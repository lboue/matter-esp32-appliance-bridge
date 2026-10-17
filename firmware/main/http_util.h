#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *body;        // NUL-terminated; caller must http_response_free()
    size_t len;
    int status_code;
    int retry_after_s; // from Retry-After, or -1 if absent
} http_response_t;

/**
 * Perform a one-shot HTTPS request and read the whole body into resp->body.
 *
 * Each call builds its own client, so concurrent callers do not share state.
 * Returns ESP_OK if the exchange completed, regardless of HTTP status code —
 * always check resp->status_code. resp->body is NULL if the body was empty.
 */
esp_err_t http_request(const char *url,
                       esp_http_client_method_t method,
                       const char *bearer_token,
                       const char *content_type,
                       const char *accept,
                       const char *body,
                       http_response_t *resp);

void http_response_free(http_response_t *resp);

/** Percent-encode `in` for an application/x-www-form-urlencoded value. */
esp_err_t http_form_encode(const char *in, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
