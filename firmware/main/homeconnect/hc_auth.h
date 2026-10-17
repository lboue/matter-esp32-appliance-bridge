#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * HomeConnect OAuth 2.0 device authorization flow and token store.
 *
 * The refresh token is persisted in NVS; the access token lives only in RAM
 * and is refreshed on boot and shortly before it expires. Tokens are never
 * logged and never returned by the web API.
 */

typedef enum {
    HC_AUTH_UNCONFIGURED, // no client ID yet
    HC_AUTH_SIGNED_OUT,   // client ID set, no refresh token
    HC_AUTH_PENDING,      // device flow started, waiting for the user
    HC_AUTH_AUTHORIZED,   // refresh token held
} hc_auth_state_t;

typedef struct {
    char user_code[24];
    char verification_uri[160];
    char verification_uri_complete[224];
    int expires_in_s;
} hc_device_flow_t;

typedef void (*hc_auth_listener_t)(hc_auth_state_t state);

esp_err_t hc_auth_init(void);

hc_auth_state_t hc_auth_state(void);
const char *hc_auth_state_name(hc_auth_state_t state);

/** Last device-flow or refresh error, for display. Empty if none. */
void hc_auth_last_error(char *buf, size_t len);

void hc_auth_get_config(char *client_id, size_t client_id_len, bool *simulator, bool *has_secret);

/**
 * Store the client registration. `client_secret` may be NULL to keep the
 * stored one, or "" to clear it. Changing the client ID or API host signs out.
 */
esp_err_t hc_auth_set_config(const char *client_id, const char *client_secret, bool simulator);

/** HC_API_BASE_PRODUCTION or HC_API_BASE_SIMULATOR. */
const char *hc_auth_api_base(void);

/**
 * Start the device flow. Blocks for one HTTPS request, then polls for the
 * user's approval on a background task.
 */
esp_err_t hc_auth_start_device_flow(hc_device_flow_t *out);

/** The in-progress device flow, if the state is HC_AUTH_PENDING. */
bool hc_auth_get_device_flow(hc_device_flow_t *out);

/**
 * Copy a valid access token into `buf`, refreshing it first if needed.
 * Blocks for an HTTPS request when a refresh is due. Never call this with the
 * CHIP stack lock held.
 */
esp_err_t hc_auth_get_access_token(char *buf, size_t len);

/** Mark the cached access token as expired, e.g. after a 401. */
void hc_auth_invalidate_access_token(void);

void hc_auth_sign_out(void);

/** Called from whichever task changed the state. Up to 4 listeners. */
void hc_auth_add_listener(hc_auth_listener_t listener);
