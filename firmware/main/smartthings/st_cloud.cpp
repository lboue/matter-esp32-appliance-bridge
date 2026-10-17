#include "st_cloud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "http_util.h"

#define TAG "st_cloud"

#define NVS_NAMESPACE "st_cloud"
#define NVS_KEY_URL "url"
#define NVS_KEY_ENROLL "enroll"
#define NVS_KEY_BRIDGE_ID "bridge_id"
#define NVS_KEY_SECRET "secret"

#define URL_LEN 128
#define ID_LEN 40
#define SECRET_LEN 64
#define TOKEN_LEN 2048
#define ERROR_LEN 128
#define MAX_MESSAGE_LEN (16 * 1024)

// The supervisor rebuilds the client if it has been down this long; the
// client's own reconnect covers ordinary drops.
#define STALE_CONNECTION_S 90
#define SUPERVISOR_PERIOD_MS 15000

static SemaphoreHandle_t s_mutex;
// Guards s_client against being destroyed mid-send. Taken with a timeout by
// senders: the client's own task sends from its event handler, and must not
// block a supervisor that is waiting for that task to stop.
static SemaphoreHandle_t s_client_mutex;
static st_event_handler_t s_on_event;
static st_state_handler_t s_on_state;
static TaskHandle_t s_supervisor;

static char s_url[URL_LEN];
static char s_enroll_token[SECRET_LEN];
static char s_bridge_id[ID_LEN];
static char s_secret[SECRET_LEN];
static char *s_token;           // access token from the relay, RAM only
static int64_t s_token_expires_us;
static bool s_linked;            // as last reported by the relay
static char s_last_error[ERROR_LEN];
static std::vector<std::string> s_devices;

static esp_websocket_client_handle_t s_client;
static volatile bool s_connected;
static volatile bool s_restart_requested;
static volatile bool s_devices_dirty;
// The first token after each connect is when SmartThings calls become possible,
// so it is reported as a state change (the bridge then re-reads devices).
static bool s_token_since_connect;
static int64_t s_down_since_us;

// Reassembly of messages split across several DATA events.
static char *s_rx;
static size_t s_rx_len;

// ---------------------------------------------------------------------------

static void lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_mutex); }

static void set_error(const char *msg)
{
    lock();
    strlcpy(s_last_error, msg, sizeof(s_last_error));
    unlock();
}

static void notify_state()
{
    if (s_on_state != NULL) {
        s_on_state(st_cloud_state(), s_connected);
    }
}

static esp_err_t nvs_write(const char *key, const char *value)
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

static void nvs_read(nvs_handle_t handle, const char *key, char *out, size_t len)
{
    size_t size = len;
    if (nvs_get_str(handle, key, out, &size) != ESP_OK) {
        out[0] = '\0';
    }
}

static void clear_token()
{
    lock();
    if (s_token != NULL) {
        memset(s_token, 0, TOKEN_LEN);
    }
    s_token_expires_us = 0;
    unlock();
}

/** wss://host/ws from the configured https://host. */
static bool websocket_uri(char *out, size_t len)
{
    lock();
    std::string url = s_url;
    unlock();
    if (url.rfind("https://", 0) == 0) {
        snprintf(out, len, "wss://%s/ws", url.c_str() + 8);
    } else if (url.rfind("http://", 0) == 0) {
        snprintf(out, len, "ws://%s/ws", url.c_str() + 7);
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Relay REST calls
// ---------------------------------------------------------------------------

/** Request against the relay, authenticated as this bridge. */
static esp_err_t relay_request(esp_http_client_method_t method, const char *path, const char *body, bool auth,
                               http_response_t *resp)
{
    char url[URL_LEN + 64];
    char secret[SECRET_LEN];
    lock();
    snprintf(url, sizeof(url), "%s%s", s_url, path);
    strlcpy(secret, s_secret, sizeof(secret));
    unlock();

    esp_err_t err = http_request(url, method, auth ? secret : NULL, body != NULL ? "application/json" : NULL,
                                 "application/json", body, resp);
    memset(secret, 0, sizeof(secret));
    if (err != ESP_OK) {
        set_error("Could not reach the SmartThings relay");
        return err;
    }
    if (resp->status_code < 200 || resp->status_code >= 300) {
        char msg[ERROR_LEN];
        cJSON *root = resp->body != NULL ? cJSON_Parse(resp->body) : NULL;
        const cJSON *detail = cJSON_GetObjectItemCaseSensitive(root, "detail");
        snprintf(msg, sizeof(msg), "Relay returned HTTP %d%s%s", resp->status_code,
                 cJSON_IsString(detail) ? ": " : "", cJSON_IsString(detail) ? detail->valuestring : "");
        cJSON_Delete(root);
        set_error(msg);
        ESP_LOGW(TAG, "%s %s", path, msg);
        return resp->status_code == 401 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

static void send_json(cJSON *message)
{
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (text == NULL) {
        return;
    }
    if (xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_client != NULL && esp_websocket_client_is_connected(s_client)) {
            esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(5000));
        }
        xSemaphoreGive(s_client_mutex);
    }
    free(text);
}

static void send_devices()
{
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "devices");
    cJSON *ids = cJSON_AddArrayToObject(message, "deviceIds");
    lock();
    for (const std::string &id : s_devices) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(id.c_str()));
    }
    unlock();
    send_json(message);
}

static void handle_message(const char *text, size_t len)
{
    cJSON *message = cJSON_ParseWithLength(text, len);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "type"));
    if (type == NULL) {
        cJSON_Delete(message);
        return;
    }

    if (strcmp(type, "event") == 0) {
        const char *device = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "deviceId"));
        const char *component = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "componentId"));
        const char *capability = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "capability"));
        const char *attribute = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "attribute"));
        // Only the main component is mapped; appliances report sub-units
        // (e.g. a hob's burners) as other components.
        if (device != NULL && capability != NULL && attribute != NULL &&
            (component == NULL || strcmp(component, "main") == 0) && s_on_event != NULL) {
            s_on_event(device, capability, attribute, cJSON_GetObjectItemCaseSensitive(message, "value"));
        }
    } else if (strcmp(type, "token") == 0) {
        const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "accessToken"));
        const cJSON *expires_in = cJSON_GetObjectItemCaseSensitive(message, "expiresIn");
        if (token != NULL && strlen(token) < TOKEN_LEN) {
            lock();
            strlcpy(s_token, token, TOKEN_LEN);
            s_token_expires_us = esp_timer_get_time() +
                                 (int64_t)(cJSON_IsNumber(expires_in) ? expires_in->valueint : 3600) * 1000000LL;
            const bool became_linked = !s_linked;
            s_linked = true;
            const bool first_since_connect = !s_token_since_connect;
            s_token_since_connect = true;
            unlock();
            ESP_LOGI(TAG, "Received SmartThings access token");
            if (became_linked || first_since_connect) {
                notify_state();
            }
        }
        if (token != NULL) {
            memset((char *)token, 0, strlen(token));
        }
    } else if (strcmp(type, "state") == 0 || strcmp(type, "linked") == 0) {
        const bool linked = strcmp(type, "linked") == 0 ||
                            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "linked"));
        lock();
        const bool changed = linked != s_linked;
        s_linked = linked;
        if (linked) {
            s_last_error[0] = '\0';
        }
        unlock();
        if (!linked) {
            clear_token();
        }
        if (changed || strcmp(type, "linked") == 0) {
            notify_state();
        }
    } else if (strcmp(type, "unlinked") == 0) {
        const char *reason = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "reason"));
        ESP_LOGW(TAG, "SmartThings unlinked: %s", reason ? reason : "");
        set_error(reason ? reason : "SmartThings was unlinked");
        lock();
        s_linked = false;
        unlock();
        clear_token();
        notify_state();
    }

    cJSON_Delete(message);
}

static void on_ws_event(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected to relay");
        s_connected = true;
        s_down_since_us = 0;
        lock();
        s_token_since_connect = false;
        unlock();
        cJSON *hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "type", "hello");
        cJSON_AddStringToObject(hello, "fw", "appliance-bridge");
        send_json(hello);
        send_devices();
        notify_state();
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_connected) {
            ESP_LOGW(TAG, "Disconnected from relay");
        }
        s_connected = false;
        if (s_down_since_us == 0) {
            s_down_since_us = esp_timer_get_time();
        }
        s_rx_len = 0;
        notify_state();
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (data != NULL && data->error_handle.esp_ws_handshake_status_code == 401) {
            set_error("The relay no longer recognises this bridge. Enrol again.");
        }
        break;
    case WEBSOCKET_EVENT_DATA: {
        // 0x1 text, 0x0 continuation; pings/pongs/close are handled by the client.
        if (data == NULL || (data->op_code != 0x1 && data->op_code != 0x0) || data->payload_len <= 0) {
            break;
        }
        if (data->payload_len > MAX_MESSAGE_LEN) {
            ESP_LOGW(TAG, "Dropping %d-byte message", data->payload_len);
            break;
        }
        if (data->payload_offset == 0) {
            s_rx_len = 0;
        }
        if (s_rx_len + data->data_len > MAX_MESSAGE_LEN || (size_t)data->payload_offset != s_rx_len) {
            s_rx_len = 0;
            break;
        }
        memcpy(s_rx + s_rx_len, data->data_ptr, data->data_len);
        s_rx_len += data->data_len;
        if (s_rx_len == (size_t)data->payload_len) {
            handle_message(s_rx, s_rx_len);
            s_rx_len = 0;
        }
        break;
    }
    default:
        break;
    }
}

static void destroy_client()
{
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    esp_websocket_client_handle_t client = s_client;
    s_client = NULL;
    xSemaphoreGive(s_client_mutex);
    if (client != NULL) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
    if (s_connected) {
        s_connected = false;
        notify_state();
    }
}

static void create_client()
{
    char uri[URL_LEN + 8];
    if (!websocket_uri(uri, sizeof(uri))) {
        set_error("The relay URL must start with https://");
        return;
    }

    char headers[SECRET_LEN + 32];
    lock();
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", s_secret);
    unlock();

    esp_websocket_client_config_t config = {};
    config.uri = uri;
    config.headers = headers;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.task_stack = 8192;
    config.reconnect_timeout_ms = 10000;
    config.network_timeout_ms = 15000;
    config.ping_interval_sec = 30;
    config.pingpong_timeout_sec = 90;

    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    memset(headers, 0, sizeof(headers));
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    s_down_since_us = esp_timer_get_time();

    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    s_client = client;
    xSemaphoreGive(s_client_mutex);
    if (esp_websocket_client_start(client) != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed");
        destroy_client();
    }
}

/** Owns the client's lifecycle, so it is never started or stopped from its own task. */
static void supervisor_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));

        const bool enrolled = st_cloud_state() >= ST_CLOUD_UNLINKED;
        const bool stale = s_client != NULL && !s_connected && s_down_since_us != 0 &&
                           esp_timer_get_time() - s_down_since_us > (int64_t)STALE_CONNECTION_S * 1000000LL;

        if (s_client != NULL && (s_restart_requested || !enrolled || stale)) {
            if (stale) {
                ESP_LOGW(TAG, "Relay connection down for %ds; recreating client", STALE_CONNECTION_S);
            }
            destroy_client();
        }
        s_restart_requested = false;

        if (s_client == NULL && enrolled) {
            create_client();
        }

        if (s_devices_dirty && s_connected) {
            s_devices_dirty = false;
            send_devices();
        }
    }
}

static void restart_connection()
{
    s_restart_requested = true;
    if (s_supervisor != NULL) {
        xTaskNotifyGive(s_supervisor);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t st_cloud_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_client_mutex = xSemaphoreCreateMutex();
    s_token = (char *)calloc(1, TOKEN_LEN);
    s_rx = (char *)malloc(MAX_MESSAGE_LEN);
    if (s_mutex == NULL || s_client_mutex == NULL || s_token == NULL || s_rx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_read(handle, NVS_KEY_URL, s_url, sizeof(s_url));
        nvs_read(handle, NVS_KEY_ENROLL, s_enroll_token, sizeof(s_enroll_token));
        nvs_read(handle, NVS_KEY_BRIDGE_ID, s_bridge_id, sizeof(s_bridge_id));
        nvs_read(handle, NVS_KEY_SECRET, s_secret, sizeof(s_secret));
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "SmartThings relay: %s", st_cloud_state_name(st_cloud_state()));
    return ESP_OK;
}

esp_err_t st_cloud_start(st_event_handler_t on_event, st_state_handler_t on_state)
{
    s_on_event = on_event;
    s_on_state = on_state;
    if (xTaskCreate(supervisor_task, "st_cloud", 4096, NULL, 4, &s_supervisor) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_supervisor);
    return ESP_OK;
}

st_cloud_state_t st_cloud_state(void)
{
    lock();
    st_cloud_state_t state = s_url[0] == '\0'      ? ST_CLOUD_UNCONFIGURED
                             : s_secret[0] == '\0' ? ST_CLOUD_UNENROLLED
                             : s_linked            ? ST_CLOUD_LINKED
                                                   : ST_CLOUD_UNLINKED;
    unlock();
    return state;
}

const char *st_cloud_state_name(st_cloud_state_t state)
{
    switch (state) {
    case ST_CLOUD_UNCONFIGURED:
        return "unconfigured";
    case ST_CLOUD_UNENROLLED:
        return "unenrolled";
    case ST_CLOUD_UNLINKED:
        return "unlinked";
    case ST_CLOUD_LINKED:
        return "linked";
    }
    return "unknown";
}

bool st_cloud_connected(void)
{
    return s_connected;
}

void st_cloud_last_error(char *buf, size_t len)
{
    lock();
    strlcpy(buf, s_last_error, len);
    unlock();
}

void st_cloud_get_config(char *url, size_t url_len, bool *has_enroll_token, char *bridge_id, size_t bridge_id_len)
{
    lock();
    if (url != NULL) {
        strlcpy(url, s_url, url_len);
    }
    if (has_enroll_token != NULL) {
        *has_enroll_token = s_enroll_token[0] != '\0';
    }
    if (bridge_id != NULL) {
        strlcpy(bridge_id, s_bridge_id, bridge_id_len);
    }
    unlock();
}

static void forget_credentials()
{
    lock();
    s_bridge_id[0] = '\0';
    memset(s_secret, 0, sizeof(s_secret));
    s_linked = false;
    unlock();
    clear_token();
    nvs_write(NVS_KEY_BRIDGE_ID, NULL);
    nvs_write(NVS_KEY_SECRET, NULL);
}

esp_err_t st_cloud_set_config(const char *url, const char *enroll_token)
{
    if (url == NULL || strlen(url) >= URL_LEN || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) ||
        (enroll_token != NULL && strlen(enroll_token) >= SECRET_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    std::string clean = url;
    while (!clean.empty() && clean.back() == '/') {
        clean.pop_back();
    }

    lock();
    const bool url_changed = clean != s_url;
    unlock();

    // An enrolment belongs to one relay.
    if (url_changed) {
        forget_credentials();
    }

    esp_err_t err = nvs_write(NVS_KEY_URL, clean.c_str());
    if (err == ESP_OK && enroll_token != NULL) {
        err = nvs_write(NVS_KEY_ENROLL, enroll_token);
    }
    if (err != ESP_OK) {
        return err;
    }

    lock();
    strlcpy(s_url, clean.c_str(), sizeof(s_url));
    if (enroll_token != NULL) {
        strlcpy(s_enroll_token, enroll_token, sizeof(s_enroll_token));
    }
    s_last_error[0] = '\0';
    unlock();

    restart_connection();
    notify_state();
    return ESP_OK;
}

esp_err_t st_cloud_enroll(void)
{
    if (st_cloud_state() == ST_CLOUD_UNCONFIGURED) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", "appliance-bridge");
    lock();
    if (s_enroll_token[0] != '\0') {
        cJSON_AddStringToObject(body, "enrollToken", s_enroll_token);
    }
    unlock();
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    http_response_t resp;
    esp_err_t err = relay_request(HTTP_METHOD_POST, "/api/bridges", json, false, &resp);
    memset(json, 0, strlen(json));
    free(json);
    if (err != ESP_OK) {
        http_response_free(&resp);
        return err;
    }

    cJSON *root = resp.body != NULL ? cJSON_Parse(resp.body) : NULL;
    http_response_free(&resp);
    const char *bridge_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "bridgeId"));
    const char *secret = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "secret"));
    if (bridge_id == NULL || secret == NULL || strlen(bridge_id) >= ID_LEN || strlen(secret) >= SECRET_LEN) {
        cJSON_Delete(root);
        set_error("The relay sent an unexpected enrolment response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = nvs_write(NVS_KEY_BRIDGE_ID, bridge_id);
    if (err == ESP_OK) {
        err = nvs_write(NVS_KEY_SECRET, secret);
    }
    if (err == ESP_OK) {
        lock();
        strlcpy(s_bridge_id, bridge_id, sizeof(s_bridge_id));
        strlcpy(s_secret, secret, sizeof(s_secret));
        s_linked = false;
        s_last_error[0] = '\0';
        unlock();
        ESP_LOGI(TAG, "Enrolled with relay as %s", bridge_id);
    }
    memset((char *)secret, 0, strlen(secret));
    cJSON_Delete(root);

    restart_connection();
    notify_state();
    return err;
}

esp_err_t st_cloud_link_url(char *url, size_t len)
{
    if (st_cloud_state() < ST_CLOUD_UNLINKED) {
        return ESP_ERR_INVALID_STATE;
    }
    http_response_t resp;
    esp_err_t err = relay_request(HTTP_METHOD_POST, "/api/bridges/me/link-code", "{}", true, &resp);
    if (err == ESP_OK) {
        cJSON *root = resp.body != NULL ? cJSON_Parse(resp.body) : NULL;
        const char *link = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "url"));
        if (link != NULL && strlen(link) < len) {
            strlcpy(url, link, len);
        } else {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        cJSON_Delete(root);
    }
    http_response_free(&resp);
    return err;
}

esp_err_t st_cloud_unlink(void)
{
    http_response_t resp;
    esp_err_t err = relay_request(HTTP_METHOD_DELETE, "/api/bridges/me/link", NULL, true, &resp);
    http_response_free(&resp);
    if (err == ESP_OK) {
        lock();
        s_linked = false;
        unlock();
        clear_token();
        notify_state();
    }
    return err;
}

esp_err_t st_cloud_forget(void)
{
    if (st_cloud_state() >= ST_CLOUD_UNLINKED) {
        http_response_t resp;
        // Best effort: forget locally even if the relay is unreachable.
        relay_request(HTTP_METHOD_DELETE, "/api/bridges/me", NULL, true, &resp);
        http_response_free(&resp);
    }
    forget_credentials();
    restart_connection();
    notify_state();
    return ESP_OK;
}

esp_err_t st_cloud_get_access_token(char *buf, size_t len)
{
    lock();
    esp_err_t err = ESP_OK;
    if (s_token[0] == '\0' || esp_timer_get_time() > s_token_expires_us) {
        err = ESP_ERR_INVALID_STATE;
    } else if (strlen(s_token) >= len) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        strlcpy(buf, s_token, len);
    }
    unlock();
    return err;
}

void st_cloud_request_token(void)
{
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "token");
    send_json(message);
}

void st_cloud_set_devices(const char *const *device_ids, size_t count)
{
    lock();
    s_devices.assign(device_ids, device_ids + count);
    unlock();
    // Sent from the supervisor: callers may hold the CHIP stack lock, and a
    // WebSocket send can block.
    s_devices_dirty = true;
    if (s_supervisor != NULL) {
        xTaskNotifyGive(s_supervisor);
    }
}
