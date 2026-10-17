#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

#include "hc_config.h"

/**
 * HomeConnect REST calls. All of these block on HTTPS, so never call them on
 * the Matter task or with the CHIP stack lock held — post them to cloud_worker.
 */

typedef struct {
    char ha_id[HC_HAID_LEN];
    char name[HC_NAME_LEN];
    char brand[HC_SHORT_LEN];
    char vib[HC_SHORT_LEN];
    char enumber[HC_SHORT_LEN];
    char type[HC_SHORT_LEN];
    bool connected;
} hc_appliance_t;

/**
 * GET {base}{path}. On a 2xx response *root_out is the parsed body, which the
 * caller must cJSON_Delete; use cJSON_GetObjectItem(root, "data") for the
 * payload. Returns ESP_ERR_NOT_FOUND for 404, ESP_ERR_INVALID_STATE for 409
 * (appliance offline or not in a state that allows the call), ESP_FAIL for
 * other HTTP errors.
 */
esp_err_t hc_client_get(const char *path, cJSON **root_out);

/** PUT {base}{path} with a JSON body. Same error mapping as hc_client_get. */
esp_err_t hc_client_put(const char *path, const char *json_body);

/** DELETE {base}{path}. Same error mapping as hc_client_get. */
esp_err_t hc_client_delete(const char *path);

/** GET /api/homeappliances. */
esp_err_t hc_client_list_appliances(hc_appliance_t *out, size_t max, size_t *count);

/** Parse one appliance object from /api/homeappliances[...]. */
void hc_client_parse_appliance(const cJSON *obj, hc_appliance_t *out);

/** The HomeConnect error description from the last failed call, for display. */
void hc_client_last_error(char *buf, size_t len);
