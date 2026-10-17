#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "cJSON.h"
#include "esp_err.h"

#include "bridge/appliance_store.h"

/**
 * SmartThings REST calls, authenticated with the access token st-relay
 * supplies. All of these block on HTTPS, so never call them on the Matter task
 * or with the CHIP stack lock held — post them to cloud_worker.
 */

#define ST_MAX_DEVICES 32

typedef struct {
    char device_id[APPLIANCE_ID_LEN];
    char label[APPLIANCE_NAME_LEN];
    char manufacturer[APPLIANCE_SHORT_LEN];
    char model[APPLIANCE_SHORT_LEN];
    bool washer; // has washerOperatingState on its main component
} st_device_t;

/** GET /v1/devices. */
esp_err_t st_client_list_devices(st_device_t *out, size_t max, size_t *count);

/** GET /v1/devices/{id}. */
esp_err_t st_client_get_device(const char *device_id, st_device_t *out);

/**
 * GET /v1/devices/{id}/status. On success *main_out is the main component's
 * { capability: { attribute: { value, ... } } } object inside *root_out,
 * which the caller must cJSON_Delete.
 */
esp_err_t st_client_get_status(const char *device_id, cJSON **root_out, const cJSON **main_out);

/** GET /v1/devices/{id}/health; *online is true for state ONLINE. */
esp_err_t st_client_get_health(const char *device_id, bool *online);

/**
 * POST /v1/devices/{id}/commands with one command on the main component.
 * `arguments_json` is a JSON array ("[\"run\"]") or NULL for none.
 */
esp_err_t st_client_command(const char *device_id, const char *capability, const char *command,
                            const char *arguments_json);

void st_client_last_error(char *buf, size_t len);

/** Parse "2026-10-16T12:34:56Z" or "...56.123Z" (UTC). */
bool st_parse_iso8601_utc(const char *iso, time_t *out);
