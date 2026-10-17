#include "hc_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hc_auth.h"
#include "http_util.h"

#define TAG "hc_client"

#define URL_LEN 256
#define ERROR_LEN 160

static portMUX_TYPE s_error_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_last_error[ERROR_LEN];

// HomeConnect rate-limits to ~1000 calls/day and 50/minute per client. After a
// 429 every call is refused locally until Retry-After has passed.
static volatile int64_t s_backoff_until_us;

static void set_last_error(const char *msg)
{
    portENTER_CRITICAL(&s_error_mux);
    strlcpy(s_last_error, msg, sizeof(s_last_error));
    portEXIT_CRITICAL(&s_error_mux);
}

void hc_client_last_error(char *buf, size_t len)
{
    portENTER_CRITICAL(&s_error_mux);
    strlcpy(buf, s_last_error, len);
    portEXIT_CRITICAL(&s_error_mux);
}

/** Record the description from HomeConnect's { "error": { key, description } }. */
static void record_api_error(int status, const char *body)
{
    char msg[ERROR_LEN];
    snprintf(msg, sizeof(msg), "HTTP %d", status);

    cJSON *root = body != NULL ? cJSON_Parse(body) : NULL;
    if (root != NULL) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(error, "key");
        const cJSON *description = cJSON_GetObjectItemCaseSensitive(error, "description");
        if (cJSON_IsString(description)) {
            snprintf(msg, sizeof(msg), "%s", description->valuestring);
        } else if (cJSON_IsString(key)) {
            snprintf(msg, sizeof(msg), "%s", key->valuestring);
        }
        cJSON_Delete(root);
    }
    set_last_error(msg);
}

static esp_err_t map_status(int status)
{
    if (status >= 200 && status < 300) {
        return ESP_OK;
    }
    switch (status) {
    case 404:
        return ESP_ERR_NOT_FOUND;
    case 409:
        return ESP_ERR_INVALID_STATE;
    case 429:
        return ESP_ERR_TIMEOUT;
    default:
        return ESP_FAIL;
    }
}

/**
 * One authenticated request, retried once with a fresh token on 401. On
 * success the response is left in *resp for the caller to free.
 */
static esp_err_t request(esp_http_client_method_t method, const char *path, const char *body, http_response_t *resp)
{
    if (esp_timer_get_time() < s_backoff_until_us) {
        set_last_error("Rate limited by HomeConnect; waiting before retrying");
        return ESP_ERR_TIMEOUT;
    }

    char url[URL_LEN];
    int n = snprintf(url, sizeof(url), "%s%s", hc_auth_api_base(), path);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *token = (char *)malloc(HC_MAX_TOKEN_LEN);
    if (token == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = hc_auth_get_access_token(token, HC_MAX_TOKEN_LEN);
        if (err != ESP_OK) {
            set_last_error("Not signed in to HomeConnect");
            break;
        }

        err = http_request(url, method, token, body != NULL ? HC_MEDIA_TYPE : NULL, HC_MEDIA_TYPE, body, resp);
        if (err != ESP_OK) {
            set_last_error("Could not reach HomeConnect");
            break;
        }

        if (resp->status_code == 401 && attempt == 0) {
            ESP_LOGW(TAG, "401 from %s; refreshing token and retrying", path);
            http_response_free(resp);
            hc_auth_invalidate_access_token();
            continue;
        }

        if (resp->status_code == 429) {
            const int wait_s = resp->retry_after_s > 0 ? resp->retry_after_s : 60;
            s_backoff_until_us = esp_timer_get_time() + (int64_t)wait_s * 1000000LL;
            ESP_LOGW(TAG, "Rate limited; backing off for %ds", wait_s);
        }

        err = map_status(resp->status_code);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s %s -> HTTP %d", method == HTTP_METHOD_GET ? "GET" : method == HTTP_METHOD_PUT ? "PUT" : "DELETE",
                     path, resp->status_code);
            record_api_error(resp->status_code, resp->body);
        }
        break;
    }

    memset(token, 0, HC_MAX_TOKEN_LEN);
    free(token);
    return err;
}

esp_err_t hc_client_get(const char *path, cJSON **root_out)
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

esp_err_t hc_client_put(const char *path, const char *json_body)
{
    http_response_t resp = {};
    esp_err_t err = request(HTTP_METHOD_PUT, path, json_body, &resp);
    http_response_free(&resp);
    return err;
}

esp_err_t hc_client_delete(const char *path)
{
    http_response_t resp = {};
    esp_err_t err = request(HTTP_METHOD_DELETE, path, NULL, &resp);
    http_response_free(&resp);
    return err;
}

static void copy_string(const cJSON *obj, const char *name, char *out, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(item)) {
        strlcpy(out, item->valuestring, len);
    } else {
        out[0] = '\0';
    }
}

void hc_client_parse_appliance(const cJSON *obj, hc_appliance_t *out)
{
    *out = {};
    copy_string(obj, "haId", out->ha_id, sizeof(out->ha_id));
    copy_string(obj, "name", out->name, sizeof(out->name));
    copy_string(obj, "brand", out->brand, sizeof(out->brand));
    copy_string(obj, "vib", out->vib, sizeof(out->vib));
    copy_string(obj, "enumber", out->enumber, sizeof(out->enumber));
    copy_string(obj, "type", out->type, sizeof(out->type));
    out->connected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, "connected"));
}

esp_err_t hc_client_list_appliances(hc_appliance_t *out, size_t max, size_t *count)
{
    *count = 0;
    cJSON *root = NULL;
    esp_err_t err = hc_client_get("/api/homeappliances", &root);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(data, "homeappliances");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, list)
    {
        if (*count >= max) {
            break;
        }
        hc_client_parse_appliance(item, &out[*count]);
        if (out[*count].ha_id[0] != '\0') {
            (*count)++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}
