#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*factory_reset_cb_t)(void);

esp_err_t web_server_start(factory_reset_cb_t on_factory_reset);

/**
 * Push a JSON event to every connected /api/events WebSocket client so the UI
 * refreshes without polling. Safe to call from any task.
 */
void web_server_notify_ws_event(const char *event_json);

#ifdef __cplusplus
}
#endif
