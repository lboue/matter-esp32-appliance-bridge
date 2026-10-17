#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/**
 * Connection to st-relay (the cloud/ service), which holds this bridge's
 * SmartThings tokens and relays device events.
 *
 * The bridge enrols once (getting a bridge ID and secret, kept in NVS), then
 * keeps a WebSocket to the relay open. Over it the relay pushes:
 *   - access tokens, used by st_client for direct SmartThings API calls;
 *     these are held in RAM only and re-sent on every connect;
 *   - device events for the devices this bridge has told it about.
 */

typedef enum {
    ST_CLOUD_UNCONFIGURED, // no relay URL
    ST_CLOUD_UNENROLLED,   // URL set, no bridge credentials
    ST_CLOUD_UNLINKED,     // enrolled, no SmartThings account linked
    ST_CLOUD_LINKED,       // SmartThings account linked
} st_cloud_state_t;

/** A device event. Runs on the WebSocket task; `value` is owned by the caller. */
typedef void (*st_event_handler_t)(const char *device_id, const char *capability, const char *attribute,
                                   const cJSON *value);

/**
 * State or connection changed, or the first access token after a (re)connect
 * arrived. Runs on whichever task changed it.
 */
typedef void (*st_state_handler_t)(st_cloud_state_t state, bool connected);

esp_err_t st_cloud_init(void);
esp_err_t st_cloud_start(st_event_handler_t on_event, st_state_handler_t on_state);

st_cloud_state_t st_cloud_state(void);
const char *st_cloud_state_name(st_cloud_state_t state);
bool st_cloud_connected(void);
void st_cloud_last_error(char *buf, size_t len);

void st_cloud_get_config(char *url, size_t url_len, bool *has_enroll_token, char *bridge_id, size_t bridge_id_len);

/**
 * Set the relay URL (https://...) and, optionally, the enrolment token. A NULL
 * token keeps the stored one. Changing the URL forgets the enrolment.
 */
esp_err_t st_cloud_set_config(const char *url, const char *enroll_token);

/** Enrol with the relay. Blocks on HTTPS; not with the CHIP stack lock held. */
esp_err_t st_cloud_enroll(void);

/** A one-time URL the user opens to link their SmartThings account. Blocks on HTTPS. */
esp_err_t st_cloud_link_url(char *url, size_t len);

/** Unlink the SmartThings account (the relay forgets its tokens). Blocks on HTTPS. */
esp_err_t st_cloud_unlink(void);

/** Remove this bridge from the relay and forget the credentials. Blocks on HTTPS. */
esp_err_t st_cloud_forget(void);

/**
 * Copy the current SmartThings access token. ESP_ERR_INVALID_STATE if the
 * relay has not supplied one (not linked, or not connected since boot).
 */
esp_err_t st_cloud_get_access_token(char *buf, size_t len);

/** Ask the relay for a fresh token, e.g. after a 401 from SmartThings. */
void st_cloud_request_token(void);

/**
 * The SmartThings devices this bridge has bridged. Sent to the relay now and
 * on every reconnect; the relay keeps one event subscription per device.
 */
void st_cloud_set_devices(const char *const *device_ids, size_t count);
