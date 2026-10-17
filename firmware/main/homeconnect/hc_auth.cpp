#include "hc_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "hc_config.h"
#include "http_util.h"

#define TAG "hc_auth"

#define NVS_NAMESPACE "hc_auth"
#define NVS_KEY_CLIENT_ID "client_id"
#define NVS_KEY_SECRET "secret"
#define NVS_KEY_SIMULATOR "sim"
#define NVS_KEY_REFRESH "refresh"

#define MAX_LISTENERS 4
#define FORM_BODY_LEN (2 * HC_MAX_TOKEN_LEN + 512)
#define DEVICE_CODE_LEN 256
#define ERROR_LEN 160

// Guards the configuration, state, device-flow details and error text. Held
// only briefly, never across an HTTPS request.
static SemaphoreHandle_t s_state_mutex;
// Serialises token refreshes so concurrent callers do not each refresh. Held
// across the HTTPS request.
static SemaphoreHandle_t s_token_mutex;

static char s_client_id[HC_MAX_CLIENT_ID_LEN];
static char s_client_secret[HC_MAX_CLIENT_SECRET_LEN];
static bool s_simulator;

// Heap buffers so ~4KB of credentials do not sit in .bss in internal RAM.
static char *s_refresh_token;
static char *s_access_token;
static int64_t s_access_expires_us; // esp_timer clock; 0 = no valid token

static hc_auth_state_t s_state = HC_AUTH_UNCONFIGURED;
static hc_device_flow_t s_flow;
static char s_device_code[DEVICE_CODE_LEN];
static int s_poll_interval_s;
static int64_t s_flow_expires_us;
static uint32_t s_flow_generation; // bumped to cancel a running poll task
static char s_last_error[ERROR_LEN];

static hc_auth_listener_t s_listeners[MAX_LISTENERS];
static size_t s_listener_count;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void lock_state() { xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
static void unlock_state() { xSemaphoreGive(s_state_mutex); }

static void notify(hc_auth_state_t state)
{
    for (size_t i = 0; i < s_listener_count; i++) {
        s_listeners[i](state);
    }
}

static void scrub_free(char *p)
{
    if (p != NULL) {
        memset(p, 0, strlen(p));
        free(p);
    }
}

static void set_error_locked(const char *msg)
{
    strlcpy(s_last_error, msg != NULL ? msg : "", sizeof(s_last_error));
}

static hc_auth_state_t derive_state_locked()
{
    if (s_client_id[0] == '\0') {
        return HC_AUTH_UNCONFIGURED;
    }
    if (s_refresh_token[0] != '\0') {
        return HC_AUTH_AUTHORIZED;
    }
    return HC_AUTH_SIGNED_OUT;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (value == NULL || value[0] == '\0') {
        err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(handle, key, value);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void nvs_read_str(nvs_handle_t handle, const char *key, char *out, size_t len)
{
    size_t size = len;
    if (nvs_get_str(handle, key, out, &size) != ESP_OK) {
        out[0] = '\0';
    }
}

/** Extract the OAuth error code from a token-endpoint response body. */
static void parse_oauth_error(const char *body, char *code, size_t code_len, char *desc, size_t desc_len)
{
    code[0] = '\0';
    desc[0] = '\0';
    cJSON *root = body != NULL ? cJSON_Parse(body) : NULL;
    if (root == NULL) {
        return;
    }
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *description = cJSON_GetObjectItemCaseSensitive(root, "error_description");
    if (cJSON_IsString(error)) {
        strlcpy(code, error->valuestring, code_len);
    } else if (cJSON_IsObject(error)) {
        // HomeConnect's API-style error envelope: { "error": { "key", "description" } }
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(error, "key");
        if (cJSON_IsString(key)) {
            strlcpy(code, key->valuestring, code_len);
        }
        description = cJSON_GetObjectItemCaseSensitive(error, "description");
    }
    if (cJSON_IsString(description)) {
        strlcpy(desc, description->valuestring, desc_len);
    }
    cJSON_Delete(root);
}

/** Store the tokens from a successful token-endpoint response. */
static esp_err_t accept_token_response(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *access = cJSON_GetObjectItemCaseSensitive(root, "access_token");
    const cJSON *refresh = cJSON_GetObjectItemCaseSensitive(root, "refresh_token");
    const cJSON *expires_in = cJSON_GetObjectItemCaseSensitive(root, "expires_in");

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsString(access) && strlen(access->valuestring) < HC_MAX_TOKEN_LEN) {
        const int expires_s = cJSON_IsNumber(expires_in) ? expires_in->valueint : 3600;

        lock_state();
        strlcpy(s_access_token, access->valuestring, HC_MAX_TOKEN_LEN);
        s_access_expires_us = esp_timer_get_time() + (int64_t)expires_s * 1000000LL;

        // HomeConnect may rotate the refresh token; persist whichever is current.
        bool refresh_changed = false;
        if (cJSON_IsString(refresh) && strlen(refresh->valuestring) < HC_MAX_TOKEN_LEN &&
            strcmp(refresh->valuestring, s_refresh_token) != 0) {
            strlcpy(s_refresh_token, refresh->valuestring, HC_MAX_TOKEN_LEN);
            refresh_changed = true;
        }
        set_error_locked("");
        unlock_state();

        if (refresh_changed) {
            err = nvs_write_str(NVS_KEY_REFRESH, refresh->valuestring);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to persist refresh token: %s", esp_err_to_name(err));
            }
        }
        ESP_LOGI(TAG, "Access token updated (expires in %ds)", expires_s);
        err = ESP_OK;
    }

    // Credentials — scrub the parsed copies before freeing.
    if (cJSON_IsString(access)) {
        memset(access->valuestring, 0, strlen(access->valuestring));
    }
    if (cJSON_IsString(refresh)) {
        memset(refresh->valuestring, 0, strlen(refresh->valuestring));
    }
    cJSON_Delete(root);
    return err;
}

/** POST a form body to /security/oauth/<endpoint>. */
static esp_err_t post_oauth(const char *endpoint, const char *form, http_response_t *resp)
{
    char url[96];
    snprintf(url, sizeof(url), "%s/security/oauth/%s", hc_auth_api_base(), endpoint);
    return http_request(url, HTTP_METHOD_POST, NULL, "application/x-www-form-urlencoded", "application/json", form,
                        resp);
}

/** Append "&name=<encoded value>" to a form body. */
static esp_err_t form_append(char *form, size_t form_len, const char *name, const char *value)
{
    const size_t used = strlen(form);
    int n = snprintf(form + used, form_len - used, "%s%s=", used > 0 ? "&" : "", name);
    if (n < 0 || (size_t)n >= form_len - used) {
        return ESP_ERR_INVALID_SIZE;
    }
    return http_form_encode(value, form + used + n, form_len - used - n);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t hc_auth_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    s_token_mutex = xSemaphoreCreateMutex();
    s_refresh_token = (char *)calloc(1, HC_MAX_TOKEN_LEN);
    s_access_token = (char *)calloc(1, HC_MAX_TOKEN_LEN);
    if (s_state_mutex == NULL || s_token_mutex == NULL || s_refresh_token == NULL || s_access_token == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_read_str(handle, NVS_KEY_CLIENT_ID, s_client_id, sizeof(s_client_id));
        nvs_read_str(handle, NVS_KEY_SECRET, s_client_secret, sizeof(s_client_secret));
        nvs_read_str(handle, NVS_KEY_REFRESH, s_refresh_token, HC_MAX_TOKEN_LEN);
        uint8_t sim = 0;
        if (nvs_get_u8(handle, NVS_KEY_SIMULATOR, &sim) == ESP_OK) {
            s_simulator = sim != 0;
        }
        nvs_close(handle);
    }

    s_state = derive_state_locked();
    ESP_LOGI(TAG, "HomeConnect auth: %s (%s API)", hc_auth_state_name(s_state),
             s_simulator ? "simulator" : "production");
    return ESP_OK;
}

hc_auth_state_t hc_auth_state(void)
{
    lock_state();
    hc_auth_state_t state = s_state;
    unlock_state();
    return state;
}

const char *hc_auth_state_name(hc_auth_state_t state)
{
    switch (state) {
    case HC_AUTH_UNCONFIGURED:
        return "unconfigured";
    case HC_AUTH_SIGNED_OUT:
        return "signed_out";
    case HC_AUTH_PENDING:
        return "pending";
    case HC_AUTH_AUTHORIZED:
        return "authorized";
    }
    return "unknown";
}

void hc_auth_last_error(char *buf, size_t len)
{
    lock_state();
    strlcpy(buf, s_last_error, len);
    unlock_state();
}

void hc_auth_get_config(char *client_id, size_t client_id_len, bool *simulator, bool *has_secret)
{
    lock_state();
    if (client_id != NULL) {
        strlcpy(client_id, s_client_id, client_id_len);
    }
    if (simulator != NULL) {
        *simulator = s_simulator;
    }
    if (has_secret != NULL) {
        *has_secret = s_client_secret[0] != '\0';
    }
    unlock_state();
}

esp_err_t hc_auth_set_config(const char *client_id, const char *client_secret, bool simulator)
{
    if (client_id == NULL || strlen(client_id) >= HC_MAX_CLIENT_ID_LEN ||
        (client_secret != NULL && strlen(client_secret) >= HC_MAX_CLIENT_SECRET_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_state();
    const bool identity_changed = strcmp(client_id, s_client_id) != 0 || simulator != s_simulator;
    unlock_state();

    // Tokens belong to one client on one API host.
    if (identity_changed) {
        hc_auth_sign_out();
    }

    esp_err_t err = nvs_write_str(NVS_KEY_CLIENT_ID, client_id);
    if (err == ESP_OK && client_secret != NULL) {
        err = nvs_write_str(NVS_KEY_SECRET, client_secret);
    }
    if (err == ESP_OK) {
        nvs_handle_t handle;
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, NVS_KEY_SIMULATOR, simulator ? 1 : 0);
            if (err == ESP_OK) {
                err = nvs_commit(handle);
            }
            nvs_close(handle);
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    lock_state();
    strlcpy(s_client_id, client_id, sizeof(s_client_id));
    if (client_secret != NULL) {
        strlcpy(s_client_secret, client_secret, sizeof(s_client_secret));
    }
    s_simulator = simulator;
    if (s_state != HC_AUTH_PENDING) {
        s_state = derive_state_locked();
    }
    const hc_auth_state_t state = s_state;
    unlock_state();

    notify(state);
    return ESP_OK;
}

const char *hc_auth_api_base(void)
{
    return s_simulator ? HC_API_BASE_SIMULATOR : HC_API_BASE_PRODUCTION;
}

// ---------------------------------------------------------------------------
// Device flow
// ---------------------------------------------------------------------------

static void device_flow_poll_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    char *form = (char *)malloc(FORM_BODY_LEN);
    if (form == NULL) {
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        lock_state();
        const int interval_s = s_poll_interval_s;
        unlock_state();
        vTaskDelay(pdMS_TO_TICKS(interval_s * 1000));

        lock_state();
        if (generation != s_flow_generation || s_state != HC_AUTH_PENDING) {
            unlock_state();
            break; // cancelled or superseded
        }
        if (esp_timer_get_time() > s_flow_expires_us) {
            set_error_locked("The sign-in code expired. Start again.");
            s_state = derive_state_locked();
            const hc_auth_state_t state = s_state;
            unlock_state();
            notify(state);
            break;
        }
        form[0] = '\0';
        form_append(form, FORM_BODY_LEN, "grant_type", "device_code");
        form_append(form, FORM_BODY_LEN, "device_code", s_device_code);
        form_append(form, FORM_BODY_LEN, "client_id", s_client_id);
        if (s_client_secret[0] != '\0') {
            form_append(form, FORM_BODY_LEN, "client_secret", s_client_secret);
        }
        unlock_state();

        http_response_t resp;
        esp_err_t err = post_oauth("token", form, &resp);
        if (err != ESP_OK) {
            continue; // network trouble; keep polling until the code expires
        }

        if (resp.status_code == 200) {
            err = accept_token_response(resp.body != NULL ? resp.body : "");
            http_response_free(&resp);

            lock_state();
            if (generation == s_flow_generation) {
                if (err == ESP_OK) {
                    s_device_code[0] = '\0';
                    s_state = HC_AUTH_AUTHORIZED;
                } else {
                    set_error_locked("HomeConnect returned an unreadable token response.");
                    s_state = derive_state_locked();
                }
            }
            const hc_auth_state_t state = s_state;
            unlock_state();
            ESP_LOGI(TAG, "Device flow finished: %s", hc_auth_state_name(state));
            notify(state);
            break;
        }

        char code[64];
        char desc[ERROR_LEN];
        parse_oauth_error(resp.body, code, sizeof(code), desc, sizeof(desc));
        const int status = resp.status_code;
        http_response_free(&resp);

        if (strcmp(code, "authorization_pending") == 0) {
            continue;
        }
        if (strcmp(code, "slow_down") == 0) {
            lock_state();
            s_poll_interval_s += 5;
            unlock_state();
            continue;
        }

        // access_denied, expired_token, or anything else is terminal.
        ESP_LOGW(TAG, "Device flow failed: HTTP %d %s", status, code);
        lock_state();
        if (generation == s_flow_generation) {
            char msg[ERROR_LEN + 32];
            snprintf(msg, sizeof(msg), "Sign-in failed: %s", desc[0] != '\0' ? desc : (code[0] ? code : "unknown"));
            set_error_locked(msg);
            s_state = derive_state_locked();
        }
        const hc_auth_state_t state = s_state;
        unlock_state();
        notify(state);
        break;
    }

    scrub_free(form);
    vTaskDelete(NULL);
}

esp_err_t hc_auth_start_device_flow(hc_device_flow_t *out)
{
    char form[256] = "";
    lock_state();
    if (s_client_id[0] == '\0') {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    form_append(form, sizeof(form), "client_id", s_client_id);
    unlock_state();
    form_append(form, sizeof(form), "scope", HC_OAUTH_SCOPE);

    http_response_t resp;
    esp_err_t err = post_oauth("device_authorization", form, &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status_code != 200 || resp.body == NULL) {
        char code[64];
        char desc[ERROR_LEN];
        parse_oauth_error(resp.body, code, sizeof(code), desc, sizeof(desc));
        ESP_LOGE(TAG, "device_authorization failed: HTTP %d %s %s", resp.status_code, code, desc);
        lock_state();
        char msg[ERROR_LEN];
        snprintf(msg, sizeof(msg), "HomeConnect rejected the request (HTTP %d%s%s)", resp.status_code,
                 code[0] ? ": " : "", code);
        set_error_locked(msg);
        unlock_state();
        http_response_free(&resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.body);
    http_response_free(&resp);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *device_code = cJSON_GetObjectItemCaseSensitive(root, "device_code");
    const cJSON *user_code = cJSON_GetObjectItemCaseSensitive(root, "user_code");
    const cJSON *uri = cJSON_GetObjectItemCaseSensitive(root, "verification_uri");
    const cJSON *uri_complete = cJSON_GetObjectItemCaseSensitive(root, "verification_uri_complete");
    const cJSON *expires_in = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "interval");

    if (!cJSON_IsString(device_code) || !cJSON_IsString(user_code) || !cJSON_IsString(uri)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    lock_state();
    s_flow = {};
    strlcpy(s_device_code, device_code->valuestring, sizeof(s_device_code));
    strlcpy(s_flow.user_code, user_code->valuestring, sizeof(s_flow.user_code));
    strlcpy(s_flow.verification_uri, uri->valuestring, sizeof(s_flow.verification_uri));
    if (cJSON_IsString(uri_complete)) {
        strlcpy(s_flow.verification_uri_complete, uri_complete->valuestring, sizeof(s_flow.verification_uri_complete));
    } else {
        strlcpy(s_flow.verification_uri_complete, uri->valuestring, sizeof(s_flow.verification_uri_complete));
    }
    s_flow.expires_in_s = cJSON_IsNumber(expires_in) ? expires_in->valueint : 300;
    s_poll_interval_s = cJSON_IsNumber(interval) && interval->valueint > 0 ? interval->valueint : 5;
    s_flow_expires_us = esp_timer_get_time() + (int64_t)s_flow.expires_in_s * 1000000LL;
    const uint32_t generation = ++s_flow_generation;
    s_state = HC_AUTH_PENDING;
    set_error_locked("");
    if (out != NULL) {
        *out = s_flow;
    }
    unlock_state();
    cJSON_Delete(root);

    if (xTaskCreate(device_flow_poll_task, "hc_auth_poll", 6144, (void *)(uintptr_t)generation, 4, NULL) != pdPASS) {
        lock_state();
        s_state = derive_state_locked();
        unlock_state();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Device flow started; waiting for the user to approve");
    notify(HC_AUTH_PENDING);
    return ESP_OK;
}

bool hc_auth_get_device_flow(hc_device_flow_t *out)
{
    lock_state();
    const bool pending = s_state == HC_AUTH_PENDING;
    if (pending && out != NULL) {
        *out = s_flow;
        const int64_t remaining_us = s_flow_expires_us - esp_timer_get_time();
        out->expires_in_s = remaining_us > 0 ? (int)(remaining_us / 1000000LL) : 0;
    }
    unlock_state();
    return pending;
}

// ---------------------------------------------------------------------------
// Access token
// ---------------------------------------------------------------------------

static esp_err_t refresh_access_token()
{
    char *form = (char *)malloc(FORM_BODY_LEN);
    if (form == NULL) {
        return ESP_ERR_NO_MEM;
    }
    form[0] = '\0';

    lock_state();
    form_append(form, FORM_BODY_LEN, "grant_type", "refresh_token");
    form_append(form, FORM_BODY_LEN, "refresh_token", s_refresh_token);
    if (s_client_secret[0] != '\0') {
        form_append(form, FORM_BODY_LEN, "client_secret", s_client_secret);
    }
    unlock_state();

    http_response_t resp;
    esp_err_t err = post_oauth("token", form, &resp);
    scrub_free(form);
    if (err != ESP_OK) {
        return err;
    }

    if (resp.status_code == 200) {
        err = accept_token_response(resp.body != NULL ? resp.body : "");
        http_response_free(&resp);
        return err;
    }

    char code[64];
    char desc[ERROR_LEN];
    parse_oauth_error(resp.body, code, sizeof(code), desc, sizeof(desc));
    const int status = resp.status_code;
    http_response_free(&resp);
    ESP_LOGE(TAG, "Token refresh failed: HTTP %d %s %s", status, code, desc);

    if (status == 400 || status == 401) {
        // The refresh token is no longer accepted: the user must sign in again.
        lock_state();
        char msg[ERROR_LEN];
        snprintf(msg, sizeof(msg), "Session expired (%s). Sign in again.", code[0] ? code : "invalid_grant");
        set_error_locked(msg);
        unlock_state();
        hc_auth_sign_out();
        // sign_out cleared the error text; restore it for the UI.
        lock_state();
        set_error_locked(msg);
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

esp_err_t hc_auth_get_access_token(char *buf, size_t len)
{
    xSemaphoreTake(s_token_mutex, portMAX_DELAY);

    lock_state();
    const bool authorized = s_state == HC_AUTH_AUTHORIZED || (s_state == HC_AUTH_PENDING && s_refresh_token[0]);
    const bool valid = s_access_token[0] != '\0' &&
                       s_access_expires_us - (int64_t)HC_TOKEN_REFRESH_MARGIN_S * 1000000LL > esp_timer_get_time();
    unlock_state();

    esp_err_t err = ESP_OK;
    if (!authorized) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!valid) {
        err = refresh_access_token();
    }

    if (err == ESP_OK) {
        lock_state();
        if (strlen(s_access_token) >= len) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            strlcpy(buf, s_access_token, len);
        }
        unlock_state();
    }

    xSemaphoreGive(s_token_mutex);
    return err;
}

void hc_auth_invalidate_access_token(void)
{
    lock_state();
    s_access_expires_us = 0;
    unlock_state();
}

void hc_auth_sign_out(void)
{
    lock_state();
    memset(s_access_token, 0, HC_MAX_TOKEN_LEN);
    memset(s_refresh_token, 0, HC_MAX_TOKEN_LEN);
    s_access_expires_us = 0;
    s_device_code[0] = '\0';
    s_flow_generation++; // cancels any poll task
    set_error_locked("");
    s_state = derive_state_locked();
    const hc_auth_state_t state = s_state;
    unlock_state();

    nvs_write_str(NVS_KEY_REFRESH, NULL);
    ESP_LOGI(TAG, "Signed out of HomeConnect");
    notify(state);
}

void hc_auth_add_listener(hc_auth_listener_t listener)
{
    if (listener != NULL && s_listener_count < MAX_LISTENERS) {
        s_listeners[s_listener_count++] = listener;
    }
}
