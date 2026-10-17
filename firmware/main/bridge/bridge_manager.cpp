#include "bridge_manager.h"

#include <inttypes.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include <esp_matter_bridge.h>
#include <esp_matter_endpoint.h>

#include "esp_log.h"
#include "nvs.h"

#include "devices/dishwasher_device.h"
#include "devices/washer_device.h"
#include "homeconnect/hc_client.h"
#include "smartthings/st_client.h"
#include "smartthings/st_cloud.h"
#include "web_server.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

#define TAG "bridge_mgr"

#define PATH_LEN 160

namespace {

class ChipLock
{
public:
    ChipLock() { chip::DeviceLayer::PlatformMgr().LockChipStack(); }
    ~ChipLock() { chip::DeviceLayer::PlatformMgr().UnlockChipStack(); }
};

void notify_ui_appliance(const char *id)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"type\":\"appliance\",\"id\":\"%s\"}", id);
    web_server_notify_ws_event(msg);
}

/** The HomeConnect appliance types this bridge knows how to map. */
bool is_supported_homeconnect_type(const char *type)
{
    return strcmp(type, "Dishwasher") == 0;
}

} // namespace

BridgeManager &BridgeManager::instance()
{
    static BridgeManager manager;
    return manager;
}

esp_err_t BridgeManager::init(node_t *node, endpoint_t *aggregator)
{
    m_node = node;
    m_aggregator_endpoint_id = endpoint::get_id(aggregator);

    esp_err_t err = esp_matter_bridge::initialize(node, device_type_callback);
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        // The endpoint ID array is sized by CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT,
        // so NVS left by firmware built with a different count (e.g. another
        // bridge project on the same board) cannot be read. Start the bridge's
        // endpoint records afresh; our own appliance list is kept and resumed
        // onto new endpoints.
        ESP_LOGW(TAG, "Bridge endpoint records are from a different build; resetting them");
        esp_matter_bridge::factory_reset();
        err = esp_matter_bridge::initialize(node, device_type_callback);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bridge: %s", esp_err_to_name(err));
        return err;
    }

    err = appliance_store_load(m_pending);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No appliances restored: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

void BridgeManager::start()
{
    ChipLock lock;

    remove_orphaned_endpoints();

    for (const StoredAppliance &stored : m_pending) {
        BridgedAppliance *appliance = make_appliance(stored);
        if (appliance == nullptr) {
            ESP_LOGW(TAG, "Skipping %s: unsupported %s type %s", stored.info.id, stored.info.source, stored.info.type);
            continue;
        }
        if (create_endpoint(appliance) == ESP_OK) {
            m_appliances.push_back(appliance);
        } else {
            delete appliance;
        }
    }
    m_pending.clear();

    // Endpoint IDs change if a resume failed and a new endpoint was created.
    save();
    publish_smartthings_devices();
    ESP_LOGI(TAG, "Bridge started with %u appliance(s)", (unsigned)m_appliances.size());
}

esp_err_t BridgeManager::device_type_callback(endpoint_t *ep, uint32_t device_type_id, void *priv_data)
{
    BridgedAppliance *appliance = static_cast<BridgedAppliance *>(priv_data);
    if (appliance == nullptr) {
        ESP_LOGE(TAG, "Bridged device created without appliance context");
        return ESP_ERR_INVALID_ARG;
    }
    if (device_type_id != appliance->device_type_id()) {
        ESP_LOGE(TAG, "Device type mismatch for %s: 0x%08" PRIx32, appliance->id(), device_type_id);
        return ESP_ERR_INVALID_ARG;
    }
    return appliance->AddClusters(ep);
}

BridgedAppliance *BridgeManager::make_appliance(const StoredAppliance &stored)
{
    if (strcmp(stored.info.source, APPLIANCE_SOURCE_HOMECONNECT) == 0 && strcmp(stored.info.type, "Dishwasher") == 0) {
        return new (std::nothrow) DishwasherDevice(stored);
    }
    if (strcmp(stored.info.source, APPLIANCE_SOURCE_SMARTTHINGS) == 0 && strcmp(stored.info.type, "Washer") == 0) {
        return new (std::nothrow) WasherDevice(stored);
    }
    return nullptr;
}

esp_err_t BridgeManager::create_endpoint(BridgedAppliance *appliance)
{
    esp_matter_bridge::device_t *dev = nullptr;
    if (appliance->endpoint_id() != chip::kInvalidEndpointId) {
        dev = esp_matter_bridge::resume_device(m_node, appliance->endpoint_id(), appliance);
        if (dev == nullptr) {
            ESP_LOGW(TAG, "Could not resume endpoint %u for %s; creating a new one", appliance->endpoint_id(),
                     appliance->id());
        }
    }
    if (dev == nullptr) {
        dev = esp_matter_bridge::create_device(m_node, m_aggregator_endpoint_id, appliance->device_type_id(),
                                               appliance);
    }
    if (dev == nullptr) {
        ESP_LOGE(TAG, "Failed to create bridged device for %s", appliance->id());
        return ESP_FAIL;
    }

    appliance->set_bridge_device(dev);
    appliance->set_endpoint_id(endpoint::get_id(dev->endpoint));

    // After esp_matter::start() this also runs the clusters' delegate init
    // callbacks for this endpoint, creating the cluster Instances.
    endpoint::enable(dev->endpoint);

    // Not reachable until the cloud says otherwise.
    esp_matter_attr_val_t reachable = esp_matter_bool(appliance->connected());
    attribute::report(appliance->endpoint_id(), BridgedDeviceBasicInformation::Id,
                      BridgedDeviceBasicInformation::Attributes::Reachable::Id, &reachable);

    appliance->OnEndpointReady();

    ESP_LOGI(TAG, "Bridged %s %s (%s) on endpoint %u", appliance->source(), appliance->stored().info.name,
             appliance->id(), appliance->endpoint_id());
    return ESP_OK;
}

void BridgeManager::remove_orphaned_endpoints()
{
    uint16_t ids[MAX_BRIDGED_DEVICE_COUNT];
    if (esp_matter_bridge::get_bridged_endpoint_ids(ids) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < MAX_BRIDGED_DEVICE_COUNT; i++) {
        if (ids[i] == chip::kInvalidEndpointId) {
            continue;
        }
        bool known = false;
        for (const StoredAppliance &stored : m_pending) {
            known |= stored.endpoint_id == ids[i];
        }
        if (!known) {
            ESP_LOGW(TAG, "Erasing orphaned bridged endpoint %u", ids[i]);
            esp_matter_bridge::erase_bridged_device_info(ids[i]);
        }
    }
}

esp_err_t BridgeManager::add_appliance(const StoredAppliance &stored, char *error, size_t error_len)
{
    BridgedAppliance *appliance = nullptr;
    {
        ChipLock lock;
        if (find(stored.info.source, stored.info.id) != nullptr) {
            snprintf(error, error_len, "Appliance is already bridged");
            return ESP_ERR_INVALID_STATE;
        }
        if (m_appliances.size() >= MAX_BRIDGED_DEVICE_COUNT) {
            snprintf(error, error_len, "The bridge is full");
            return ESP_ERR_NO_MEM;
        }
        appliance = make_appliance(stored);
        if (appliance == nullptr) {
            snprintf(error, error_len, "Out of memory");
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = create_endpoint(appliance);
        if (err != ESP_OK) {
            delete appliance;
            snprintf(error, error_len, "Could not create the Matter endpoint");
            return err;
        }
        m_appliances.push_back(appliance);
        appliance->SetConnected(true);
        save();
        publish_smartthings_devices();
    }

    appliance->RequestRefresh();
    web_server_notify_ws_event("{\"type\":\"bridge_changed\"}");
    return ESP_OK;
}

esp_err_t BridgeManager::add_homeconnect_appliance(const char *ha_id, char *error, size_t error_len)
{
    error[0] = '\0';
    {
        ChipLock lock;
        if (find(APPLIANCE_SOURCE_HOMECONNECT, ha_id) != nullptr) {
            snprintf(error, error_len, "Appliance is already bridged");
            return ESP_ERR_INVALID_STATE;
        }
    }

    char path[PATH_LEN];
    snprintf(path, sizeof(path), "/api/homeappliances/%s", ha_id);
    cJSON *root = NULL;
    esp_err_t err = hc_client_get(path, &root);
    if (err != ESP_OK) {
        char detail[96];
        hc_client_last_error(detail, sizeof(detail));
        snprintf(error, error_len, "Could not read the appliance: %s", detail);
        return err;
    }

    hc_appliance_t hc;
    hc_client_parse_appliance(cJSON_GetObjectItemCaseSensitive(root, "data"), &hc);
    cJSON_Delete(root);

    if (!is_supported_homeconnect_type(hc.type)) {
        snprintf(error, error_len, "%s appliances are not supported yet", hc.type);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!hc.connected) {
        // The program list (and so the Matter mode list) can only be read
        // while the appliance is online.
        snprintf(error, error_len, "The appliance is offline. Switch it on and try again.");
        return ESP_ERR_INVALID_STATE;
    }

    StoredAppliance stored;
    strlcpy(stored.info.source, APPLIANCE_SOURCE_HOMECONNECT, sizeof(stored.info.source));
    strlcpy(stored.info.id, hc.ha_id, sizeof(stored.info.id));
    strlcpy(stored.info.name, hc.name, sizeof(stored.info.name));
    strlcpy(stored.info.brand, hc.brand, sizeof(stored.info.brand));
    strlcpy(stored.info.model, hc.vib, sizeof(stored.info.model));
    strlcpy(stored.info.type, hc.type, sizeof(stored.info.type));

    snprintf(path, sizeof(path), "/api/homeappliances/%s/programs/available", ha_id);
    err = hc_client_get(path, &root);
    if (err != ESP_OK) {
        char detail[96];
        hc_client_last_error(detail, sizeof(detail));
        snprintf(error, error_len, "Could not read the program list: %s", detail);
        return err;
    }
    const cJSON *program = NULL;
    cJSON_ArrayForEach(program, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "data"), "programs"))
    {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(program, "key");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(program, "name");
        if (cJSON_IsString(key) && strlen(key->valuestring) < HC_PROGRAM_KEY_LEN &&
            stored.programs.size() < HC_MAX_PROGRAMS) {
            stored.programs.push_back({key->valuestring, cJSON_IsString(name) ? name->valuestring : ""});
        }
    }
    cJSON_Delete(root);

    if (stored.programs.empty()) {
        snprintf(error, error_len, "HomeConnect reported no programs for this appliance");
        return ESP_ERR_NOT_FOUND;
    }

    return add_appliance(stored, error, error_len);
}

esp_err_t BridgeManager::add_smartthings_appliance(const char *device_id, char *error, size_t error_len)
{
    error[0] = '\0';
    st_device_t device;
    esp_err_t err = st_client_get_device(device_id, &device);
    if (err != ESP_OK) {
        char detail[96];
        st_client_last_error(detail, sizeof(detail));
        snprintf(error, error_len, "Could not read the device: %s", detail);
        return err;
    }
    if (!device.washer) {
        snprintf(error, error_len, "Only SmartThings washers are supported so far");
        return ESP_ERR_NOT_SUPPORTED;
    }

    StoredAppliance stored;
    strlcpy(stored.info.source, APPLIANCE_SOURCE_SMARTTHINGS, sizeof(stored.info.source));
    strlcpy(stored.info.id, device.device_id, sizeof(stored.info.id));
    strlcpy(stored.info.name, device.label, sizeof(stored.info.name));
    strlcpy(stored.info.brand, device.manufacturer, sizeof(stored.info.brand));
    strlcpy(stored.info.model, device.model, sizeof(stored.info.model));
    strlcpy(stored.info.type, "Washer", sizeof(stored.info.type));

    return add_appliance(stored, error, error_len);
}

esp_err_t BridgeManager::remove_appliance(const char *source, const char *id)
{
    ChipLock lock;
    for (auto it = m_appliances.begin(); it != m_appliances.end(); ++it) {
        BridgedAppliance *appliance = *it;
        if (!appliance->is(source, id)) {
            continue;
        }
        esp_err_t err = esp_matter_bridge::remove_device(
            static_cast<esp_matter_bridge::device_t *>(appliance->bridge_device()));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to remove endpoint %u: %s", appliance->endpoint_id(), esp_err_to_name(err));
        }
        appliance->set_bridge_device(nullptr);
        m_appliances.erase(it);
        m_retired.push_back(appliance);
        save();
        publish_smartthings_devices();
        ESP_LOGI(TAG, "Unbridged %s %s", source, id);
        web_server_notify_ws_event("{\"type\":\"bridge_changed\"}");
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void BridgeManager::refresh_all(const char *source)
{
    ChipLock lock;
    for (BridgedAppliance *appliance : m_appliances) {
        if (source == nullptr || strcmp(appliance->source(), source) == 0) {
            appliance->RequestRefresh();
        }
    }
}

// ---- HomeConnect ----

void BridgeManager::on_event(const char *event, const char *ha_id, const cJSON *data)
{
    if (strcmp(event, "PAIRED") == 0 || strcmp(event, "DEPAIRED") == 0) {
        web_server_notify_ws_event("{\"type\":\"appliances_changed\"}");
        return;
    }

    {
        ChipLock lock;
        BridgedAppliance *appliance = find(APPLIANCE_SOURCE_HOMECONNECT, ha_id);
        if (appliance == nullptr) {
            return; // an appliance on the account that is not bridged
        }

        if (strcmp(event, "CONNECTED") == 0) {
            appliance->SetConnected(true);
            // Whatever changed while it was offline was not streamed.
            appliance->RequestRefresh();
        } else if (strcmp(event, "DISCONNECTED") == 0) {
            appliance->SetConnected(false);
        } else if (strcmp(event, "STATUS") == 0 || strcmp(event, "NOTIFY") == 0 || strcmp(event, "EVENT") == 0) {
            appliance->ApplyEvent(event, data);
        }
    }
    notify_ui_appliance(ha_id);
}

void BridgeManager::on_stream_state(bool connected)
{
    web_server_notify_ws_event(connected ? "{\"type\":\"stream\",\"connected\":true}"
                                         : "{\"type\":\"stream\",\"connected\":false}");
    if (connected) {
        // Catch up on anything that happened while the stream was down.
        refresh_all(APPLIANCE_SOURCE_HOMECONNECT);
    }
}

// ---- SmartThings ----

void BridgeManager::on_st_event(const char *device_id, const char *capability, const char *attribute,
                                const cJSON *value)
{
    {
        ChipLock lock;
        BridgedAppliance *appliance = find(APPLIANCE_SOURCE_SMARTTHINGS, device_id);
        if (appliance == nullptr) {
            return;
        }
        // An event means SmartThings can reach the device.
        appliance->SetConnected(true);
        appliance->ApplyAttribute(capability, attribute, value);
        appliance->Sync();
    }
    notify_ui_appliance(device_id);
}

void BridgeManager::on_st_state(bool linked, bool connected)
{
    char msg[80];
    snprintf(msg, sizeof(msg), "{\"type\":\"smartthings\",\"linked\":%s,\"connected\":%s}", linked ? "true" : "false",
             connected ? "true" : "false");
    web_server_notify_ws_event(msg);
}

void BridgeManager::publish_smartthings_devices()
{
    std::vector<const char *> ids;
    for (BridgedAppliance *appliance : m_appliances) {
        if (strcmp(appliance->source(), APPLIANCE_SOURCE_SMARTTHINGS) == 0) {
            ids.push_back(appliance->id());
        }
    }
    st_cloud_set_devices(ids.data(), ids.size());
}

// ---- Common ----

esp_err_t BridgeManager::on_attribute_write(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                            esp_matter_attr_val_t *val)
{
    BridgedAppliance *appliance = find_by_endpoint(endpoint_id);
    if (appliance == nullptr) {
        return ESP_OK;
    }
    return appliance->OnAttributeWrite(cluster_id, attribute_id, val);
}

BridgedAppliance *BridgeManager::find(const char *source, const char *id)
{
    for (BridgedAppliance *appliance : m_appliances) {
        if (appliance->is(source, id)) {
            return appliance;
        }
    }
    return nullptr;
}

BridgedAppliance *BridgeManager::find_by_endpoint(uint16_t endpoint_id)
{
    for (BridgedAppliance *appliance : m_appliances) {
        if (appliance->endpoint_id() == endpoint_id) {
            return appliance;
        }
    }
    return nullptr;
}

void BridgeManager::save()
{
    std::vector<const StoredAppliance *> stored;
    stored.reserve(m_appliances.size());
    for (BridgedAppliance *appliance : m_appliances) {
        stored.push_back(&appliance->stored());
    }
    appliance_store_save(stored);
}

void BridgeManager::to_json(cJSON *array)
{
    ChipLock lock;
    for (BridgedAppliance *appliance : m_appliances) {
        cJSON *obj = cJSON_CreateObject();
        appliance->ToJson(obj);
        cJSON_AddItemToArray(array, obj);
    }
}
