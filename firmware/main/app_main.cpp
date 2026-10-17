#include <stdio.h>

#include <esp_matter.h>
#include <esp_matter_bridge.h>
#include <nvs_flash.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "mdns.h"
#include "nvs.h"

#include "bridge/bridge_manager.h"
#include "homeconnect/hc_auth.h"
#include "homeconnect/hc_events.h"
#include "cloud_worker.h"
#include "smartthings/st_cloud.h"
#include "web_server.h"

#define MDNS_HOSTNAME "appliance-bridge"
#define MDNS_HTTP_PORT 80

#define ABORT_APP_ON_FAILURE(x, ...)               \
    do {                                           \
        if (!(unlikely(x))) {                      \
            __VA_ARGS__;                           \
            vTaskDelay(5000 / portTICK_PERIOD_MS); \
            abort();                               \
        }                                          \
    } while (0)

#define TAG "app_main"

using namespace esp_matter;
using namespace esp_matter::endpoint;

static void register_mdns(void)
{
    static bool registered = false;
    if (registered) {
        return;
    }

    esp_netif_t *eth_netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t ip_info;
    if (eth_netif == nullptr || esp_netif_get_ip_info(eth_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return;
    }

    mdns_ip_addr_t addr = {};
    addr.addr.type = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4 = ip_info.ip;
    addr.next = nullptr;

    if (mdns_delegate_hostname_add(MDNS_HOSTNAME, &addr) != ESP_OK ||
        mdns_service_add_for_host(NULL, "_http", "_tcp", MDNS_HOSTNAME, MDNS_HTTP_PORT, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "mDNS registration failed");
        return;
    }

    registered = true;
    ESP_LOGI(TAG, "Web UI at http://%s.local (" IPSTR ")", MDNS_HOSTNAME, IP2STR(&ip_info.ip));
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        web_server_notify_ws_event("{\"type\":\"commissioned\"}");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        web_server_notify_ws_event("{\"type\":\"window_closed\"}");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        web_server_notify_ws_event("{\"type\":\"fabric_removed\"}");
        break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        register_mdns();
        // The stream task waits for HomeConnect auth on its own; this just
        // gets it moving as soon as there is an address.
        hc_events_reconnect();
        break;
    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    // Writes from a controller (e.g. OnOff) are forwarded to HomeConnect.
    // Updates that came from HomeConnect are pushed with attribute::report(),
    // which does not call back here.
    if (type == attribute::PRE_UPDATE) {
        return BridgeManager::instance().on_attribute_write(endpoint_id, cluster_id, attribute_id, val);
    }
    return ESP_OK;
}

static void on_auth_changed(hc_auth_state_t state)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"type\":\"auth\",\"state\":\"%s\"}", hc_auth_state_name(state));
    web_server_notify_ws_event(msg);
    hc_events_reconnect();
}

static void on_stream_event(const char *event, const char *ha_id, const cJSON *data)
{
    BridgeManager::instance().on_event(event, ha_id, data);
}

static void on_stream_state(bool connected)
{
    BridgeManager::instance().on_stream_state(connected);
}

static void on_st_event(const char *device_id, const char *capability, const char *attribute, const cJSON *value)
{
    BridgeManager::instance().on_st_event(device_id, capability, attribute, value);
}

static void on_st_state(st_cloud_state_t state, bool connected)
{
    const bool linked = state == ST_CLOUD_LINKED;
    BridgeManager::instance().on_st_state(linked, connected);

    // Events are not queued while the relay connection is down, so re-read
    // the washers once SmartThings calls are possible again.
    char probe[8];
    const esp_err_t token = st_cloud_get_access_token(probe, sizeof(probe));
    if (linked && connected && (token == ESP_OK || token == ESP_ERR_INVALID_SIZE)) {
        BridgeManager::instance().refresh_all(APPLIANCE_SOURCE_SMARTTHINGS);
    }
}

static void erase_namespace(const char *ns)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void factory_reset(void)
{
    // Forget the HomeConnect account and the bridged appliances as well as
    // the Matter fabrics.
    erase_namespace("hc_auth");
    erase_namespace("appliances");
    erase_namespace("st_cloud");
    esp_matter_bridge::factory_reset();
    esp_matter::factory_reset();
}

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ABORT_APP_ON_FAILURE(hc_auth_init() == ESP_OK, ESP_LOGE(TAG, "Failed to init HomeConnect auth"));
    ABORT_APP_ON_FAILURE(st_cloud_init() == ESP_OK, ESP_LOGE(TAG, "Failed to init SmartThings relay"));
    hc_auth_add_listener(on_auth_changed);

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    aggregator::config_t aggregator_config;
    endpoint_t *aggregator = aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(aggregator != nullptr, ESP_LOGE(TAG, "Failed to create aggregator endpoint"));

    ABORT_APP_ON_FAILURE(BridgeManager::instance().init(node, aggregator) == ESP_OK,
                         ESP_LOGE(TAG, "Failed to init bridge"));

    // Ethernet is brought up by CHIP calling ESPEthernetDriver::Init during
    // esp_matter::start(); see matter_ethernet_driver.cpp.
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter: %d", err));

    // Bridged endpoints are rebuilt after start so endpoint::enable() runs
    // their cluster init callbacks.
    BridgeManager::instance().start();

    ABORT_APP_ON_FAILURE(cloud_worker_start() == ESP_OK, ESP_LOGE(TAG, "Failed to start cloud worker"));
    ABORT_APP_ON_FAILURE(hc_events_start(on_stream_event, on_stream_state) == ESP_OK,
                         ESP_LOGE(TAG, "Failed to start HomeConnect event stream"));

    ABORT_APP_ON_FAILURE(st_cloud_start(on_st_event, on_st_state) == ESP_OK,
                         ESP_LOGE(TAG, "Failed to start SmartThings relay connection"));

    // Wall-clock time, for the washer's countdown (SmartThings reports an
    // absolute completionTime). SNTP retries until the network is up.
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);

    web_server_start(factory_reset);
}
