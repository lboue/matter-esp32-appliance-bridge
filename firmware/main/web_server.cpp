#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "bridge/bridge_manager.h"
#include "homeconnect/hc_auth.h"
#include "homeconnect/hc_client.h"
#include "homeconnect/hc_events.h"
#include "smartthings/st_client.h"
#include "smartthings/st_cloud.h"

#define TAG "web_server"

#define MAX_BODY_LEN 1024
#define COMMISSIONING_WINDOW_SECONDS 180
#define WS_MAX_CLIENTS 4

// The React app, embedded by main/CMakeLists.txt (target_add_binary_data).
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
extern const uint8_t app_css_end[] asm("_binary_app_css_end");

static factory_reset_cb_t s_on_factory_reset;

// esp_timer clock reading of when the commissioning window was opened, so
// /api/matter/pairing can report the seconds remaining.
static volatile int64_t s_window_open_time_us = 0;

static httpd_handle_t s_ws_hd = NULL;
static int s_ws_fds[WS_MAX_CLIENTS];
static portMUX_TYPE s_ws_mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Serialize `root`, send it as application/json with `status`, and free both. */
static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status = "200 OK")
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(req, ok);
}

/** Send { "error": msg } with an explicit status line. */
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root, status);
}

/** Read and parse a bounded JSON request body. NULL on any failure. */
static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY_LEN) {
        return NULL;
    }
    char *body = (char *)malloc(req->content_len + 1);
    if (body == NULL) {
        return NULL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return NULL;
        }
        received += ret;
    }
    body[received] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    return root;
}

/** The URI segment after `prefix`, without any query string. */
static bool uri_tail(httpd_req_t *req, const char *prefix, char *out, size_t len)
{
    const size_t prefix_len = strlen(prefix);
    if (strncmp(req->uri, prefix, prefix_len) != 0) {
        return false;
    }
    const char *tail = req->uri + prefix_len;
    size_t n = strcspn(tail, "?");
    if (n == 0 || n >= len) {
        return false;
    }
    memcpy(out, tail, n);
    out[n] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Static files
// ---------------------------------------------------------------------------

static esp_err_t send_embedded(httpd_req_t *req, const char *type, const uint8_t *start, const uint8_t *end,
                               bool cache)
{
    httpd_resp_set_type(req, type);
    // index.html must be revalidated so a new firmware's bundle is picked up;
    // app.js/app.css have fixed names, so they get a short cache only.
    httpd_resp_set_hdr(req, "Cache-Control", cache ? "max-age=300" : "no-cache");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strncmp(uri, "/app.js", 7) == 0) {
        return send_embedded(req, "application/javascript", app_js_start, app_js_end, true);
    }
    if (strncmp(uri, "/app.css", 8) == 0) {
        return send_embedded(req, "text/css", app_css_start, app_css_end, true);
    }
    if (strncmp(uri, "/api/", 5) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    // Everything else is a client-side route of the single-page app.
    return send_embedded(req, "text/html", index_html_start, index_html_end, false);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (eth != NULL && esp_netif_get_ip_info(eth, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(network, "ipv4", ip);
    }

    uint8_t fabric_count = 0;
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    cJSON *matter = cJSON_AddObjectToObject(root, "matter");
    cJSON_AddNumberToObject(matter, "fabrics", fabric_count);

    cJSON *hc = cJSON_AddObjectToObject(root, "homeconnect");
    char error[160];
    hc_auth_last_error(error, sizeof(error));
    bool simulator = false;
    hc_auth_get_config(NULL, 0, &simulator, NULL);
    cJSON_AddStringToObject(hc, "state", hc_auth_state_name(hc_auth_state()));
    cJSON_AddStringToObject(hc, "error", error);
    cJSON_AddBoolToObject(hc, "simulator", simulator);
    cJSON_AddBoolToObject(hc, "streamConnected", hc_events_connected());

    cJSON *st = cJSON_AddObjectToObject(root, "smartthings");
    char st_error[128];
    st_cloud_last_error(st_error, sizeof(st_error));
    cJSON_AddStringToObject(st, "state", st_cloud_state_name(st_cloud_state()));
    cJSON_AddBoolToObject(st, "relayConnected", st_cloud_connected());
    cJSON_AddStringToObject(st, "error", st_error);

    cJSON *system = cJSON_AddObjectToObject(root, "system");
    cJSON_AddNumberToObject(system, "uptimeSeconds", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(system, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(system, "minFreeHeap", esp_get_minimum_free_heap_size());

    return send_json(req, root);
}

// ---------------------------------------------------------------------------
// HomeConnect account
// ---------------------------------------------------------------------------

static esp_err_t hc_config_get_handler(httpd_req_t *req)
{
    char client_id[HC_MAX_CLIENT_ID_LEN];
    bool simulator = false;
    bool has_secret = false;
    hc_auth_get_config(client_id, sizeof(client_id), &simulator, &has_secret);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "clientId", client_id);
    cJSON_AddBoolToObject(root, "simulator", simulator);
    // The secret itself is never returned.
    cJSON_AddBoolToObject(root, "hasClientSecret", has_secret);
    return send_json(req, root);
}

static esp_err_t hc_config_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    if (root == NULL) {
        return send_error(req, "400 Bad Request", "Invalid JSON");
    }

    const cJSON *client_id = cJSON_GetObjectItemCaseSensitive(root, "clientId");
    const cJSON *client_secret = cJSON_GetObjectItemCaseSensitive(root, "clientSecret");
    const cJSON *simulator = cJSON_GetObjectItemCaseSensitive(root, "simulator");

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(client_id) && client_id->valuestring[0] != '\0') {
        // clientSecret absent = keep the stored one; "" = clear it.
        err = hc_auth_set_config(client_id->valuestring,
                                 cJSON_IsString(client_secret) ? client_secret->valuestring : NULL,
                                 cJSON_IsTrue(simulator));
    }
    if (cJSON_IsString(client_secret)) {
        memset(client_secret->valuestring, 0, strlen(client_secret->valuestring));
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "A client ID is required");
    }
    // A different client or API host means a different event stream.
    hc_events_reconnect();
    return send_ok(req);
}

static cJSON *device_flow_json(const hc_device_flow_t &flow)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "userCode", flow.user_code);
    cJSON_AddStringToObject(root, "verificationUri", flow.verification_uri);
    cJSON_AddStringToObject(root, "verificationUriComplete", flow.verification_uri_complete);
    cJSON_AddNumberToObject(root, "expiresIn", flow.expires_in_s);
    return root;
}

static esp_err_t hc_login_get_handler(httpd_req_t *req)
{
    hc_device_flow_t flow;
    if (!hc_auth_get_device_flow(&flow)) {
        return send_error(req, "404 Not Found", "No sign-in in progress");
    }
    return send_json(req, device_flow_json(flow));
}

static esp_err_t hc_login_post_handler(httpd_req_t *req)
{
    hc_device_flow_t flow;
    esp_err_t err = hc_auth_start_device_flow(&flow);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "Set a client ID first");
    }
    if (err != ESP_OK) {
        char error[160];
        hc_auth_last_error(error, sizeof(error));
        return send_error(req, "502 Bad Gateway", error[0] ? error : "Could not reach HomeConnect");
    }
    return send_json(req, device_flow_json(flow));
}

static esp_err_t hc_logout_handler(httpd_req_t *req)
{
    hc_auth_sign_out();
    hc_events_reconnect();
    return send_ok(req);
}

static esp_err_t hc_appliances_get_handler(httpd_req_t *req)
{
    hc_appliance_t *list = (hc_appliance_t *)calloc(HC_MAX_APPLIANCES * 2, sizeof(hc_appliance_t));
    if (list == NULL) {
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    size_t count = 0;
    esp_err_t err = hc_client_list_appliances(list, HC_MAX_APPLIANCES * 2, &count);
    if (err != ESP_OK) {
        free(list);
        char detail[128];
        hc_client_last_error(detail, sizeof(detail));
        return send_error(req, "502 Bad Gateway", detail);
    }

    cJSON *root = cJSON_CreateArray();
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    for (size_t i = 0; i < count; i++) {
        const hc_appliance_t &a = list[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "haId", a.ha_id);
        cJSON_AddStringToObject(obj, "name", a.name);
        cJSON_AddStringToObject(obj, "brand", a.brand);
        cJSON_AddStringToObject(obj, "vib", a.vib);
        cJSON_AddStringToObject(obj, "type", a.type);
        cJSON_AddBoolToObject(obj, "connected", a.connected);
        cJSON_AddBoolToObject(obj, "bridged",
                              BridgeManager::instance().find(APPLIANCE_SOURCE_HOMECONNECT, a.ha_id) != nullptr);
        cJSON_AddBoolToObject(obj, "supported", strcmp(a.type, "Dishwasher") == 0);
        cJSON_AddItemToArray(root, obj);
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    free(list);
    return send_json(req, root);
}

// ---------------------------------------------------------------------------
// Bridged appliances
// ---------------------------------------------------------------------------

static esp_err_t bridge_list_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateArray();
    BridgeManager::instance().to_json(root);
    return send_json(req, root);
}

/** Body { "source": "homeconnect" | "smartthings", "id": "..." }. */
static esp_err_t bridge_add_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    const char *source = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "source"));
    const char *id_value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id"));
    if (source == NULL || id_value == NULL || id_value[0] == '\0' || strlen(id_value) >= APPLIANCE_ID_LEN ||
        (strcmp(source, APPLIANCE_SOURCE_HOMECONNECT) != 0 && strcmp(source, APPLIANCE_SOURCE_SMARTTHINGS) != 0)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "source and id are required");
    }
    char id[APPLIANCE_ID_LEN];
    strlcpy(id, id_value, sizeof(id));
    const bool homeconnect = strcmp(source, APPLIANCE_SOURCE_HOMECONNECT) == 0;
    cJSON_Delete(root);

    char error[160];
    esp_err_t err = homeconnect ? BridgeManager::instance().add_homeconnect_appliance(id, error, sizeof(error))
                                : BridgeManager::instance().add_smartthings_appliance(id, error, sizeof(error));
    if (err != ESP_OK) {
        return send_error(req, err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_SUPPORTED ? "409 Conflict"
                                                                                           : "502 Bad Gateway",
                          error);
    }
    return send_ok(req);
}

/** DELETE /api/bridge/appliances/{source}/{id} */
static esp_err_t bridge_remove_handler(httpd_req_t *req)
{
    char tail[APPLIANCE_SOURCE_LEN + APPLIANCE_ID_LEN + 1];
    char *slash = NULL;
    if (!uri_tail(req, "/api/bridge/appliances/", tail, sizeof(tail)) || (slash = strchr(tail, '/')) == NULL) {
        return send_error(req, "400 Bad Request", "Expected /api/bridge/appliances/{source}/{id}");
    }
    *slash = '\0';
    if (BridgeManager::instance().remove_appliance(tail, slash + 1) != ESP_OK) {
        return send_error(req, "404 Not Found", "Appliance is not bridged");
    }
    return send_ok(req);
}

static esp_err_t bridge_refresh_handler(httpd_req_t *req)
{
    BridgeManager::instance().refresh_all();
    return send_ok(req);
}

// ---------------------------------------------------------------------------
// SmartThings (through the st-relay cloud service)
// ---------------------------------------------------------------------------

static esp_err_t st_config_get_handler(httpd_req_t *req)
{
    char url[128];
    char bridge_id[40];
    bool has_enroll_token = false;
    st_cloud_get_config(url, sizeof(url), &has_enroll_token, bridge_id, sizeof(bridge_id));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "relayUrl", url);
    cJSON_AddBoolToObject(root, "hasEnrollToken", has_enroll_token);
    cJSON_AddStringToObject(root, "bridgeId", bridge_id);
    return send_json(req, root);
}

static esp_err_t st_config_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "relayUrl"));
    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "enrollToken");
    // enrollToken absent = keep the stored one; "" = clear it.
    esp_err_t err = url != NULL ? st_cloud_set_config(url, cJSON_IsString(token) ? token->valuestring : NULL)
                                : ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(token)) {
        memset(token->valuestring, 0, strlen(token->valuestring));
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "relayUrl must be an https:// URL");
    }
    return send_ok(req);
}

static esp_err_t st_relay_error(httpd_req_t *req, esp_err_t err)
{
    char error[128];
    st_cloud_last_error(error, sizeof(error));
    return send_error(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "502 Bad Gateway",
                      error[0] ? error : esp_err_to_name(err));
}

static esp_err_t st_enroll_handler(httpd_req_t *req)
{
    esp_err_t err = st_cloud_enroll();
    return err == ESP_OK ? send_ok(req) : st_relay_error(req, err);
}

static esp_err_t st_link_handler(httpd_req_t *req)
{
    char url[256];
    esp_err_t err = st_cloud_link_url(url, sizeof(url));
    if (err != ESP_OK) {
        return st_relay_error(req, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "url", url);
    return send_json(req, root);
}

static esp_err_t st_unlink_handler(httpd_req_t *req)
{
    esp_err_t err = st_cloud_unlink();
    return err == ESP_OK ? send_ok(req) : st_relay_error(req, err);
}

static esp_err_t st_forget_handler(httpd_req_t *req)
{
    st_cloud_forget();
    return send_ok(req);
}

static esp_err_t st_devices_handler(httpd_req_t *req)
{
    st_device_t *list = (st_device_t *)calloc(ST_MAX_DEVICES, sizeof(st_device_t));
    if (list == NULL) {
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    size_t count = 0;
    esp_err_t err = st_client_list_devices(list, ST_MAX_DEVICES, &count);
    if (err != ESP_OK) {
        free(list);
        char detail[128];
        st_client_last_error(detail, sizeof(detail));
        return send_error(req, "502 Bad Gateway", detail);
    }

    cJSON *root = cJSON_CreateArray();
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    for (size_t i = 0; i < count; i++) {
        const st_device_t &d = list[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", d.device_id);
        cJSON_AddStringToObject(obj, "name", d.label);
        cJSON_AddStringToObject(obj, "brand", d.manufacturer);
        cJSON_AddStringToObject(obj, "model", d.model);
        cJSON_AddStringToObject(obj, "type", d.washer ? "Washer" : "Other");
        cJSON_AddBoolToObject(obj, "supported", d.washer);
        cJSON_AddBoolToObject(obj, "bridged",
                              BridgeManager::instance().find(APPLIANCE_SOURCE_SMARTTHINGS, d.device_id) != nullptr);
        cJSON_AddItemToArray(root, obj);
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    free(list);
    return send_json(req, root);
}

// ---------------------------------------------------------------------------
// Matter commissioning + fabrics
// ---------------------------------------------------------------------------

static esp_err_t matter_pairing_handler(httpd_req_t *req)
{
    char qr_buf[128] = {};
    char manual_buf[32] = {};

    struct FabricEntry {
        chip::FabricIndex index;
        chip::VendorId vendorId;
        chip::NodeId nodeId;
        char label[chip::kFabricLabelMaxLengthInBytes + 1];
    };
    FabricEntry fabric_entries[CHIP_CONFIG_MAX_FABRICS] = {};
    uint8_t fabric_count = 0;

    chip::DeviceLayer::PlatformMgr().LockChipStack();

    const bool window_open =
        chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();

    // BLE is disabled: this is an on-network (DNS-SD) commissionable device.
    chip::RendezvousInformationFlags rendezvous(chip::RendezvousInformationFlag::kOnNetwork);
    chip::MutableCharSpan qr_span(qr_buf, sizeof(qr_buf) - 1);
    chip::MutableCharSpan manual_span(manual_buf, sizeof(manual_buf) - 1);
    if (GetQRCode(qr_span, rendezvous) != CHIP_NO_ERROR) {
        qr_buf[0] = '\0';
    }
    if (GetManualPairingCode(manual_span, rendezvous) != CHIP_NO_ERROR) {
        manual_buf[0] = '\0';
    }

    for (const auto &fabric : chip::Server::GetInstance().GetFabricTable()) {
        if (fabric_count >= CHIP_CONFIG_MAX_FABRICS) {
            break;
        }
        auto &e = fabric_entries[fabric_count++];
        e.index = fabric.GetFabricIndex();
        e.vendorId = fabric.GetVendorId();
        e.nodeId = fabric.GetNodeId();
        auto label = fabric.GetFabricLabel();
        size_t len = label.size() < sizeof(e.label) - 1 ? label.size() : sizeof(e.label) - 1;
        memcpy(e.label, label.data(), len);
        e.label[len] = '\0';
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    int window_seconds_remaining = 0;
    if (window_open) {
        if (s_window_open_time_us > 0) {
            int64_t elapsed = (esp_timer_get_time() - s_window_open_time_us) / 1000000LL;
            int remaining = COMMISSIONING_WINDOW_SECONDS - (int)elapsed;
            window_seconds_remaining = remaining > 0 ? remaining : 0;
        } else {
            // Opened at boot by the stack (no fabrics yet); length unknown here.
            window_seconds_remaining = COMMISSIONING_WINDOW_SECONDS;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "qrCode", qr_buf);
    cJSON_AddStringToObject(root, "manualPairingCode", manual_buf);
    cJSON_AddBoolToObject(root, "windowOpen", window_open);
    cJSON_AddNumberToObject(root, "windowSecondsRemaining", window_seconds_remaining);

    cJSON *fabrics = cJSON_AddArrayToObject(root, "fabrics");
    for (uint8_t i = 0; i < fabric_count; i++) {
        const auto &e = fabric_entries[i];
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "fabricIndex", e.index);
        cJSON_AddNumberToObject(f, "vendorId", static_cast<uint16_t>(e.vendorId));
        cJSON_AddStringToObject(f, "label", e.label);
        char node_id[20];
        snprintf(node_id, sizeof(node_id), "0x%016llX", (unsigned long long)e.nodeId);
        cJSON_AddStringToObject(f, "nodeId", node_id);
        cJSON_AddItemToArray(fabrics, f);
    }

    return send_json(req, root);
}

static esp_err_t matter_open_window_handler(httpd_req_t *req)
{
    // Opening the window touches the CHIP stack, so do it on the Matter task.
    CHIP_ERROR scheduled = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t) {
            auto &manager = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (manager.IsCommissioningWindowOpen()) {
                manager.CloseCommissioningWindow();
            }
            CHIP_ERROR err = manager.OpenBasicCommissioningWindow(
                chip::System::Clock::Seconds32(COMMISSIONING_WINDOW_SECONDS));
            if (err != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
            } else {
                s_window_open_time_us = esp_timer_get_time();
                ESP_LOGI(TAG, "Commissioning window opened (%ds)", COMMISSIONING_WINDOW_SECONDS);
                web_server_notify_ws_event("{\"type\":\"window_opened\"}");
            }
        },
        0);
    if (scheduled != CHIP_NO_ERROR) {
        return send_error(req, "500 Internal Server Error", "Could not open the commissioning window");
    }
    return send_ok(req);
}

static esp_err_t matter_remove_fabric_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req);
    const cJSON *index = cJSON_GetObjectItemCaseSensitive(root, "index");
    if (!cJSON_IsNumber(index) || index->valueint < 1 || index->valueint > 254) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "Missing or invalid fabric index");
    }
    const chip::FabricIndex fabric_index = static_cast<chip::FabricIndex>(index->valueint);
    cJSON_Delete(root);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    CHIP_ERROR err = chip::Server::GetInstance().GetFabricTable().Delete(fabric_index);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Fabric %u delete failed: %" CHIP_ERROR_FORMAT, fabric_index, err.Format());
        return send_error(req, "404 Not Found", "No such fabric");
    }
    ESP_LOGI(TAG, "Fabric %u removed", fabric_index);
    return send_ok(req);
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

static void restart_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    send_ok(req);
    // Let the response go out first.
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    send_ok(req);
    if (s_on_factory_reset != NULL) {
        s_on_factory_reset();
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// WebSocket /api/events
// ---------------------------------------------------------------------------

static void ws_forget(int fd)
{
    portENTER_CRITICAL(&s_ws_mux);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = -1;
        }
    }
    portEXIT_CRITICAL(&s_ws_mux);
}

static esp_err_t ws_events_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        portENTER_CRITICAL(&s_ws_mux);
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (s_ws_fds[i] < 0) {
                s_ws_fds[i] = fd;
                s_ws_hd = req->handle;
                break;
            }
        }
        portEXIT_CRITICAL(&s_ws_mux);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.len > 0 && frame.len < 256) {
        uint8_t buf[256];
        frame.payload = buf;
        httpd_ws_recv_frame(req, &frame, frame.len);
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_forget(httpd_req_to_sockfd(req));
    }
    return ESP_OK;
}

void web_server_notify_ws_event(const char *event_json)
{
    if (s_ws_hd == NULL) {
        return;
    }

    int fds[WS_MAX_CLIENTS];
    portENTER_CRITICAL(&s_ws_mux);
    memcpy(fds, s_ws_fds, sizeof(fds));
    portEXIT_CRITICAL(&s_ws_mux);

    httpd_ws_frame_t frame = {};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)event_json;
    frame.len = strlen(event_json);

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (fds[i] < 0) {
            continue;
        }
        if (httpd_ws_get_fd_info(s_ws_hd, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET ||
            httpd_ws_send_frame_async(s_ws_hd, fds[i], &frame) != ESP_OK) {
            ws_forget(fds[i]);
        }
    }
}

// ---------------------------------------------------------------------------

esp_err_t web_server_start(factory_reset_cb_t on_factory_reset)
{
    s_on_factory_reset = on_factory_reset;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_ws_fds[i] = -1;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.stack_size = 10240;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
        return err;
    }

    // Routes match in registration order, so the "/*" catch-all goes last.
    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/api/homeconnect/config", .method = HTTP_GET, .handler = hc_config_get_handler},
        {.uri = "/api/homeconnect/config", .method = HTTP_POST, .handler = hc_config_post_handler},
        {.uri = "/api/homeconnect/login", .method = HTTP_GET, .handler = hc_login_get_handler},
        {.uri = "/api/homeconnect/login", .method = HTTP_POST, .handler = hc_login_post_handler},
        {.uri = "/api/homeconnect/logout", .method = HTTP_POST, .handler = hc_logout_handler},
        {.uri = "/api/homeconnect/appliances", .method = HTTP_GET, .handler = hc_appliances_get_handler},
        {.uri = "/api/bridge/appliances", .method = HTTP_GET, .handler = bridge_list_handler},
        {.uri = "/api/bridge/appliances", .method = HTTP_POST, .handler = bridge_add_handler},
        {.uri = "/api/bridge/appliances/*", .method = HTTP_DELETE, .handler = bridge_remove_handler},
        {.uri = "/api/bridge/refresh", .method = HTTP_POST, .handler = bridge_refresh_handler},
        {.uri = "/api/smartthings/config", .method = HTTP_GET, .handler = st_config_get_handler},
        {.uri = "/api/smartthings/config", .method = HTTP_POST, .handler = st_config_post_handler},
        {.uri = "/api/smartthings/enroll", .method = HTTP_POST, .handler = st_enroll_handler},
        {.uri = "/api/smartthings/link", .method = HTTP_POST, .handler = st_link_handler},
        {.uri = "/api/smartthings/unlink", .method = HTTP_POST, .handler = st_unlink_handler},
        {.uri = "/api/smartthings/forget", .method = HTTP_POST, .handler = st_forget_handler},
        {.uri = "/api/smartthings/devices", .method = HTTP_GET, .handler = st_devices_handler},
        {.uri = "/api/matter/pairing", .method = HTTP_GET, .handler = matter_pairing_handler},
        {.uri = "/api/matter/open-commissioning-window", .method = HTTP_POST, .handler = matter_open_window_handler},
        {.uri = "/api/matter/remove-fabric", .method = HTTP_POST, .handler = matter_remove_fabric_handler},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_handler},
        {.uri = "/api/events", .method = HTTP_GET, .handler = ws_events_handler, .user_ctx = NULL, .is_websocket = true},
        {.uri = "/*", .method = HTTP_GET, .handler = static_get_handler},
    };

    for (const auto &route : routes) {
        err = httpd_register_uri_handler(server, &route);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", route.uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}
