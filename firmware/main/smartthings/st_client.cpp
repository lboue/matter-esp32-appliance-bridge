#include "st_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "http_util.h"
#include "st_cloud.h"

#define TAG "st_client"

#define API_BASE "https://api.smartthings.com/v1"
#define TOKEN_LEN 2048
#define URL_LEN 192
#define ERROR_LEN 128

static portMUX_TYPE s_error_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_last_error[ERROR_LEN];

static void set_last_error(const char *msg)
{
    portENTER_CRITICAL(&s_error_mux);
    strlcpy(s_last_error, msg, sizeof(s_last_error));
    portEXIT_CRITICAL(&s_error_mux);
}

void st_client_last_error(char *buf, size_t len)
{
    portENTER_CRITICAL(&s_error_mux);
    strlcpy(buf, s_last_error, len);
    portEXIT_CRITICAL(&s_error_mux);
}

/**
 * One authenticated request. On a 401 the relay is asked for a fresh token and
 * the request is retried once. The response is left in *resp for the caller.
 */
static esp_err_t request(esp_http_client_method_t method, const char *path, const char *body, http_response_t *resp)
{
    char url[URL_LEN];
    snprintf(url, sizeof(url), API_BASE "%s", path);

    char *token = (char *)malloc(TOKEN_LEN);
    if (token == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = st_cloud_get_access_token(token, TOKEN_LEN);
        if (err != ESP_OK) {
            set_last_error("No SmartThings token yet: link SmartThings and check the relay connection");
            break;
        }
        err = http_request(url, method, token, body != NULL ? "application/json" : NULL, "application/json", body,
                           resp);
        if (err != ESP_OK) {
            set_last_error("Could not reach SmartThings");
            break;
        }
        if (resp->status_code == 401 && attempt == 0) {
            ESP_LOGW(TAG, "401 from %s; asking the relay for a new token", path);
            http_response_free(resp);
            st_cloud_request_token();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (resp->status_code < 200 || resp->status_code >= 300) {
            char msg[ERROR_LEN];
            snprintf(msg, sizeof(msg), "SmartThings returned HTTP %d", resp->status_code);
            cJSON *root = resp->body != NULL ? cJSON_Parse(resp->body) : NULL;
            const char *message =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "error"), "message"));
            if (message != NULL) {
                snprintf(msg, sizeof(msg), "%s", message);
            }
            cJSON_Delete(root);
            set_last_error(msg);
            ESP_LOGW(TAG, "%s -> HTTP %d", path, resp->status_code);
            err = resp->status_code == 404 ? ESP_ERR_NOT_FOUND : resp->status_code == 429 ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        break;
    }

    memset(token, 0, TOKEN_LEN);
    free(token);
    return err;
}

static esp_err_t get_json(const char *path, cJSON **root_out)
{
    *root_out = NULL;
    http_response_t resp = {};
    esp_err_t err = request(HTTP_METHOD_GET, path, NULL, &resp);
    if (err == ESP_OK) {
        *root_out = resp.body != NULL ? cJSON_Parse(resp.body) : NULL;
        if (*root_out == NULL) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    http_response_free(&resp);
    return err;
}

static bool main_has_capability(const cJSON *device, const char *capability)
{
    const cJSON *component = NULL;
    cJSON_ArrayForEach(component, cJSON_GetObjectItemCaseSensitive(device, "components"))
    {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(component, "id"));
        if (id == NULL || strcmp(id, "main") != 0) {
            continue;
        }
        const cJSON *cap = NULL;
        cJSON_ArrayForEach(cap, cJSON_GetObjectItemCaseSensitive(component, "capabilities"))
        {
            const char *cap_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cap, "id"));
            if (cap_id != NULL && strcmp(cap_id, capability) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void copy_first(char *out, size_t len, const char *a, const char *b)
{
    const char *value = (a != NULL && a[0] != '\0') ? a : b;
    strlcpy(out, value != NULL ? value : "", len);
}

static void parse_device(const cJSON *item, st_device_t *out)
{
    *out = {};
    copy_first(out->device_id, sizeof(out->device_id),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "deviceId")), NULL);
    // `label` is the user's name for the device; `name` is the vendor default.
    copy_first(out->label, sizeof(out->label), cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "label")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "name")));

    // Samsung appliances describe themselves in the OCF block.
    const cJSON *ocf = cJSON_GetObjectItemCaseSensitive(item, "ocf");
    copy_first(out->manufacturer, sizeof(out->manufacturer),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ocf, "manufacturerName")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "manufacturerName")));
    copy_first(out->model, sizeof(out->model), cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ocf, "modelNumber")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "deviceTypeName")));

    out->washer = main_has_capability(item, "washerOperatingState");
}

esp_err_t st_client_list_devices(st_device_t *out, size_t max, size_t *count)
{
    *count = 0;
    cJSON *root = NULL;
    esp_err_t err = get_json("/devices", &root);
    if (err != ESP_OK) {
        return err;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "items"))
    {
        if (*count >= max) {
            break;
        }
        parse_device(item, &out[*count]);
        if (out[*count].device_id[0] != '\0') {
            (*count)++;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t st_client_get_device(const char *device_id, st_device_t *out)
{
    char path[96];
    snprintf(path, sizeof(path), "/devices/%s", device_id);
    cJSON *root = NULL;
    esp_err_t err = get_json(path, &root);
    if (err == ESP_OK) {
        parse_device(root, out);
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t st_client_get_status(const char *device_id, cJSON **root_out, const cJSON **main_out)
{
    char path[96];
    snprintf(path, sizeof(path), "/devices/%s/status", device_id);
    esp_err_t err = get_json(path, root_out);
    if (err != ESP_OK) {
        return err;
    }
    *main_out = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(*root_out, "components"), "main");
    return *main_out != NULL ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t st_client_get_health(const char *device_id, bool *online)
{
    char path[96];
    snprintf(path, sizeof(path), "/devices/%s/health", device_id);
    cJSON *root = NULL;
    esp_err_t err = get_json(path, &root);
    if (err == ESP_OK) {
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "state"));
        *online = state != NULL && strcmp(state, "ONLINE") == 0;
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t st_client_command(const char *device_id, const char *capability, const char *command,
                            const char *arguments_json)
{
    char path[96];
    snprintf(path, sizeof(path), "/devices/%s/commands", device_id);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"commands\":[{\"component\":\"main\",\"capability\":\"%s\",\"command\":\"%s\",\"arguments\":%s}]}",
             capability, command, arguments_json != NULL ? arguments_json : "[]");

    http_response_t resp = {};
    esp_err_t err = request(HTTP_METHOD_POST, path, body, &resp);
    http_response_free(&resp);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: %s.%s%s accepted", device_id, capability, command, arguments_json ? arguments_json : "");
    }
    return err;
}

/** Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm). */
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

bool st_parse_iso8601_utc(const char *iso, time_t *out)
{
    if (iso == NULL) {
        return false;
    }
    int y, mo, d, h, mi, s;
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6 || mo < 1 || mo > 12 || d < 1 ||
        d > 31) {
        return false;
    }
    *out = (time_t)(days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400L + h * 3600L + mi * 60L + s);
    return true;
}
