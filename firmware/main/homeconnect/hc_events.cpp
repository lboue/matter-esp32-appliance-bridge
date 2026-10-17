#include "hc_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hc_auth.h"
#include "hc_config.h"

#define TAG "hc_events"

#define TASK_STACK 8192
#define LINE_MAX_LEN 8192
#define DATA_MAX_LEN 16384
#define EVENT_NAME_LEN 32
// Short read timeout so the loop can notice reconnect requests and idle
// timeouts; it does not bound how long the stream stays open.
#define READ_TIMEOUT_MS 2000

static TaskHandle_t s_task;
static hc_event_handler_t s_on_event;
static hc_stream_state_handler_t s_on_state;
static volatile bool s_connected;
static volatile bool s_reconnect_requested;

typedef struct {
    char *line;
    size_t line_len;
    bool line_overflow;

    char *data;
    size_t data_len;
    bool data_overflow;

    char event[EVENT_NAME_LEN];
    char id[HC_HAID_LEN];
} sse_parser_t;

static void set_connected(bool connected)
{
    if (s_connected != connected) {
        s_connected = connected;
        ESP_LOGI(TAG, "Event stream %s", connected ? "connected" : "disconnected");
        if (s_on_state != NULL) {
            s_on_state(connected);
        }
    }
}

static void parser_reset_event(sse_parser_t *p)
{
    p->data_len = 0;
    p->data[0] = '\0';
    p->data_overflow = false;
    p->event[0] = '\0';
    p->id[0] = '\0';
}

static void dispatch_event(sse_parser_t *p)
{
    if (p->event[0] == '\0' && p->data_len == 0) {
        return;
    }
    if (strcmp(p->event, "KEEP-ALIVE") == 0) {
        ESP_LOGD(TAG, "keep-alive");
        return;
    }
    if (p->data_overflow) {
        ESP_LOGW(TAG, "Dropping oversized %s event", p->event);
        return;
    }

    cJSON *data = p->data_len > 0 ? cJSON_Parse(p->data) : NULL;

    // The SSE id carries the haId; fall back to the payload's haId field.
    const char *ha_id = p->id;
    if (ha_id[0] == '\0' && data != NULL) {
        const cJSON *field = cJSON_GetObjectItemCaseSensitive(data, "haId");
        if (cJSON_IsString(field)) {
            ha_id = field->valuestring;
        }
    }

    ESP_LOGI(TAG, "%s for %s (%u bytes)", p->event[0] ? p->event : "message", ha_id, (unsigned)p->data_len);
    if (s_on_event != NULL) {
        s_on_event(p->event[0] ? p->event : "message", ha_id, data);
    }
    cJSON_Delete(data);
}

static const char *field_value(const char *line, size_t prefix_len)
{
    const char *value = line + prefix_len;
    return (*value == ' ') ? value + 1 : value;
}

static void process_line(sse_parser_t *p)
{
    const char *line = p->line;

    if (p->line_len == 0) {
        dispatch_event(p);
        parser_reset_event(p);
        return;
    }
    if (p->line_overflow) {
        // A truncated data line would corrupt the JSON; poison the event.
        if (strncmp(line, "data:", 5) == 0) {
            p->data_overflow = true;
        }
        return;
    }
    if (line[0] == ':') {
        return; // comment
    }

    if (strncmp(line, "event:", 6) == 0) {
        strlcpy(p->event, field_value(line, 6), sizeof(p->event));
    } else if (strncmp(line, "id:", 3) == 0) {
        strlcpy(p->id, field_value(line, 3), sizeof(p->id));
    } else if (strncmp(line, "data:", 5) == 0) {
        const char *value = field_value(line, 5);
        const size_t value_len = strlen(value);
        const size_t sep = p->data_len > 0 ? 1 : 0;
        if (p->data_len + sep + value_len + 1 > DATA_MAX_LEN) {
            p->data_overflow = true;
            return;
        }
        if (sep) {
            p->data[p->data_len++] = '\n';
        }
        memcpy(p->data + p->data_len, value, value_len);
        p->data_len += value_len;
        p->data[p->data_len] = '\0';
    }
}

static void parser_feed(sse_parser_t *p, char c)
{
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        p->line[p->line_len] = '\0';
        process_line(p);
        p->line_len = 0;
        p->line_overflow = false;
        return;
    }
    if (p->line_len + 1 >= LINE_MAX_LEN) {
        p->line_overflow = true;
        return;
    }
    p->line[p->line_len++] = c;
}

/**
 * Open the stream and read until it drops, times out or a reconnect is
 * requested. Returns the HTTP status (0 if the connection failed) and the
 * Retry-After hint, if any.
 */
static int run_stream(sse_parser_t *parser, const char *token, int *retry_after_s)
{
    *retry_after_s = -1;

    char url[96];
    snprintf(url, sizeof(url), "%s/api/homeappliances/events", hc_auth_api_base());

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = READ_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return 0;
    }

    const size_t auth_len = strlen(token) + sizeof("Bearer ");
    char *auth = (char *)malloc(auth_len);
    if (auth == NULL) {
        esp_http_client_cleanup(client);
        return 0;
    }
    snprintf(auth, auth_len, "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "text/event-stream");

    int status = 0;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
    }

    if (status == 200) {
        if (!esp_http_client_is_chunked_response(client)) {
            // esp_http_client_read only streams chunked bodies of unknown length.
            ESP_LOGW(TAG, "Event stream is not chunked; events may not be delivered");
        }

        set_connected(true);
        parser->line_len = 0;
        parser->line_overflow = false;
        parser_reset_event(parser);
        int64_t last_rx_us = esp_timer_get_time();

        while (!s_reconnect_requested) {
            // One byte at a time: esp_http_client_read keeps reading until its
            // buffer is full, so a larger read would hold back the tail of an
            // event until the next one arrived. Event traffic is light.
            char c;
            int r = esp_http_client_read(client, &c, 1);
            if (r == 1) {
                last_rx_us = esp_timer_get_time();
                parser_feed(parser, c);
                continue;
            }
            if (r == -ESP_ERR_HTTP_EAGAIN) {
                if (esp_timer_get_time() - last_rx_us > (int64_t)HC_SSE_IDLE_TIMEOUT_S * 1000000LL) {
                    ESP_LOGW(TAG, "No data for %ds; reconnecting", HC_SSE_IDLE_TIMEOUT_S);
                    break;
                }
                continue;
            }
            ESP_LOGW(TAG, "Event stream closed (%d)", r);
            break;
        }
    } else if (status != 0) {
        char *value = NULL;
        if (esp_http_client_get_header(client, "Retry-After", &value) == ESP_OK && value != NULL) {
            *retry_after_s = atoi(value);
        }
    }

    memset(auth, 0, auth_len);
    free(auth);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

static void events_task(void *)
{
    sse_parser_t parser = {};
    parser.line = (char *)malloc(LINE_MAX_LEN);
    parser.data = (char *)malloc(DATA_MAX_LEN);
    char *token = (char *)malloc(HC_MAX_TOKEN_LEN);
    if (parser.line == NULL || parser.data == NULL || token == NULL) {
        ESP_LOGE(TAG, "Out of memory; event stream disabled");
        vTaskDelete(NULL);
        return;
    }

    int backoff_s = HC_SSE_BACKOFF_MIN_S;

    while (true) {
        s_reconnect_requested = false;

        if (hc_auth_state() != HC_AUTH_AUTHORIZED) {
            set_connected(false);
            // Woken early by hc_events_reconnect() once the user signs in.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
            continue;
        }

        esp_err_t err = hc_auth_get_access_token(token, HC_MAX_TOKEN_LEN);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "No access token (%s); retrying in %ds", esp_err_to_name(err), backoff_s);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff_s * 1000));
            backoff_s = backoff_s * 2 > HC_SSE_BACKOFF_MAX_S ? HC_SSE_BACKOFF_MAX_S : backoff_s * 2;
            continue;
        }

        const int64_t started_us = esp_timer_get_time();
        int retry_after_s = -1;
        const int status = run_stream(&parser, token, &retry_after_s);
        memset(token, 0, HC_MAX_TOKEN_LEN);
        set_connected(false);

        if (s_reconnect_requested) {
            continue;
        }

        if (status == 401) {
            ESP_LOGW(TAG, "401 on event stream; refreshing token");
            hc_auth_invalidate_access_token();
        }

        // A stream that stayed up for a while was healthy; reconnect promptly.
        if (status == 200 && esp_timer_get_time() - started_us > 60LL * 1000000LL) {
            backoff_s = HC_SSE_BACKOFF_MIN_S;
        }

        int wait_s = backoff_s;
        if (retry_after_s > wait_s) {
            wait_s = retry_after_s;
        }
        ESP_LOGI(TAG, "Reconnecting event stream in %ds (last status %d)", wait_s, status);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000));
        backoff_s = backoff_s * 2 > HC_SSE_BACKOFF_MAX_S ? HC_SSE_BACKOFF_MAX_S : backoff_s * 2;
    }
}

esp_err_t hc_events_start(hc_event_handler_t on_event, hc_stream_state_handler_t on_state)
{
    s_on_event = on_event;
    s_on_state = on_state;
    if (xTaskCreate(events_task, "hc_events", TASK_STACK, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool hc_events_connected(void)
{
    return s_connected;
}

void hc_events_reconnect(void)
{
    s_reconnect_requested = true;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}
