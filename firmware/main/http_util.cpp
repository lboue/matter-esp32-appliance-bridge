#include "http_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"

#define TAG "http_util"

// HomeConnect payloads (status, settings, program lists) run to a few KB.
// Anything beyond this is a sign something is wrong, so cap rather than grow
// without bound.
#define HTTP_MAX_BODY_LEN (64 * 1024)
#define HTTP_READ_CHUNK 1024
#define HTTP_TIMEOUT_MS 15000

void http_response_free(http_response_t *resp)
{
    if (resp == NULL) {
        return;
    }
    free(resp->body);
    resp->body = NULL;
    resp->len = 0;
}

static int read_retry_after(esp_http_client_handle_t client)
{
    char *value = NULL;
    if (esp_http_client_get_header(client, "Retry-After", &value) != ESP_OK || value == NULL) {
        return -1;
    }
    return atoi(value);
}

esp_err_t http_request(const char *url,
                       esp_http_client_method_t method,
                       const char *bearer_token,
                       const char *content_type,
                       const char *accept,
                       const char *body,
                       http_response_t *resp)
{
    if (url == NULL || resp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *resp = {};
    resp->status_code = -1;
    resp->retry_after_s = -1;

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = method;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    // HomeConnect access tokens are JWTs well over the default 512-byte TX buffer.
    config.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed for %s", url);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    char *auth_header = NULL;

    if (bearer_token != NULL) {
        size_t auth_len = strlen(bearer_token) + sizeof("Bearer ");
        auth_header = (char *)malloc(auth_len);
        if (auth_header == NULL) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        snprintf(auth_header, auth_len, "Bearer %s", bearer_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }

    if (content_type != NULL) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    esp_http_client_set_header(client, "Accept", accept != NULL ? accept : "application/json");
    // HomeConnect only includes display names (program names, appliance names)
    // when a language is requested.
    esp_http_client_set_header(client, "Accept-Language", "en-GB");

    {
        const size_t body_len = (body != NULL) ? strlen(body) : 0;

        err = esp_http_client_open(client, body_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open connection to %s: %s", url, esp_err_to_name(err));
            goto cleanup;
        }

        if (body_len > 0) {
            int written = esp_http_client_write(client, body, body_len);
            if (written < 0 || (size_t)written != body_len) {
                ESP_LOGE(TAG, "Failed to write request body (%d of %u)", written, (unsigned)body_len);
                err = ESP_FAIL;
                goto cleanup;
            }
        }

        int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            ESP_LOGE(TAG, "Failed to fetch headers from %s", url);
            err = ESP_FAIL;
            goto cleanup;
        }

        resp->status_code = esp_http_client_get_status_code(client);
        resp->retry_after_s = read_retry_after(client);

        // content_length is 0 for chunked responses, so grow as we read rather
        // than trusting it.
        size_t capacity = (content_length > 0 && content_length < HTTP_MAX_BODY_LEN)
                              ? (size_t)content_length + 1
                              : HTTP_READ_CHUNK + 1;
        char *buf = (char *)malloc(capacity);
        if (buf == NULL) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        size_t total = 0;
        while (true) {
            if (total + HTTP_READ_CHUNK + 1 > capacity) {
                size_t new_capacity = capacity * 2;
                if (new_capacity > HTTP_MAX_BODY_LEN + 1) {
                    new_capacity = HTTP_MAX_BODY_LEN + 1;
                }
                if (new_capacity <= total + 1) {
                    ESP_LOGE(TAG, "Response from %s exceeds %d bytes; truncating", url, HTTP_MAX_BODY_LEN);
                    break;
                }
                char *grown = (char *)realloc(buf, new_capacity);
                if (grown == NULL) {
                    free(buf);
                    err = ESP_ERR_NO_MEM;
                    goto cleanup;
                }
                buf = grown;
                capacity = new_capacity;
            }

            int read_len = esp_http_client_read(client, buf + total, capacity - total - 1);
            if (read_len < 0) {
                ESP_LOGE(TAG, "Read error from %s", url);
                free(buf);
                err = ESP_FAIL;
                goto cleanup;
            }
            if (read_len == 0) {
                break; // complete
            }
            total += (size_t)read_len;
        }

        buf[total] = '\0';
        resp->body = buf;
        resp->len = total;
    }

cleanup:
    if (auth_header != NULL) {
        // Credential — scrub before freeing.
        memset(auth_header, 0, strlen(auth_header));
        free(auth_header);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        http_response_free(resp);
    }
    return err;
}

esp_err_t http_form_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        const bool unreserved = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                                *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (unreserved) {
            if (o + 1 >= out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    if (out_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    out[o] = '\0';
    return ESP_OK;
}
