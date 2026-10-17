#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

/**
 * HomeConnect Server-Sent Events client.
 *
 * Keeps one stream open to /api/homeappliances/events, which carries events
 * for every appliance on the account. Each event is handed to `on_event` on
 * the stream task:
 *   - event: STATUS, NOTIFY, EVENT, CONNECTED, DISCONNECTED, PAIRED, DEPAIRED
 *   - ha_id: the appliance the event is about
 *   - data:  the parsed data payload ({ "items": [ { key, value, ... } ] }),
 *            or NULL if it was empty. Owned by the caller of the handler.
 * KEEP-ALIVE events are consumed internally.
 */

typedef void (*hc_event_handler_t)(const char *event, const char *ha_id, const cJSON *data);
typedef void (*hc_stream_state_handler_t)(bool connected);

esp_err_t hc_events_start(hc_event_handler_t on_event, hc_stream_state_handler_t on_state);

bool hc_events_connected(void);

/** Drop the current stream (if any) and reconnect, e.g. after signing in. */
void hc_events_reconnect(void);
