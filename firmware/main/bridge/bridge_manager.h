#pragma once

#include <stddef.h>
#include <vector>

#include <esp_matter.h>

#include "cJSON.h"

#include "devices/bridged_appliance.h"

/**
 * Owns the bridged appliances: one Matter endpoint per cloud appliance
 * (HomeConnect or SmartThings), under the aggregator.
 *
 * The CHIP stack lock is the bridge's lock. Appliances are only added or
 * removed while holding it, so code that holds it may use the pointers
 * returned by find().
 */
class BridgeManager
{
public:
    static BridgeManager &instance();

    /** Load the stored appliance list. Call before esp_matter::start(). */
    esp_err_t init(esp_matter::node_t *node, esp_matter::endpoint_t *aggregator);

    /** Recreate the stored appliances' endpoints. Call after esp_matter::start(). */
    void start();

    /**
     * Bridge an appliance. Each blocks on HTTPS to fetch the appliance's
     * details, so must not be called with the CHIP stack lock held.
     */
    esp_err_t add_homeconnect_appliance(const char *ha_id, char *error, size_t error_len);
    esp_err_t add_smartthings_appliance(const char *device_id, char *error, size_t error_len);

    /** Remove an appliance's endpoint. Must not be called with the lock held. */
    esp_err_t remove_appliance(const char *source, const char *id);

    /** Ask appliances to fetch a fresh snapshot; `source` NULL means all. Any thread. */
    void refresh_all(const char *source = nullptr);

    /** HomeConnect event-stream callbacks (stream task). */
    void on_event(const char *event, const char *ha_id, const cJSON *data);
    void on_stream_state(bool connected);

    /** SmartThings relay callbacks (relay WebSocket task). */
    void on_st_event(const char *device_id, const char *capability, const char *attribute, const cJSON *value);
    void on_st_state(bool linked, bool connected);

    /** Route a controller's attribute write (Matter task, lock held). */
    esp_err_t on_attribute_write(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                 esp_matter_attr_val_t *val);

    // ---- Lock held ----
    BridgedAppliance *find(const char *source, const char *id);
    BridgedAppliance *find_by_endpoint(uint16_t endpoint_id);
    const std::vector<BridgedAppliance *> &appliances() const { return m_appliances; }
    void save();

    /** Append every appliance's state to `array`. Takes the lock. */
    void to_json(cJSON *array);

private:
    BridgeManager() = default;

    static esp_err_t device_type_callback(esp_matter::endpoint_t *ep, uint32_t device_type_id, void *priv_data);

    BridgedAppliance *make_appliance(const StoredAppliance &stored);
    esp_err_t create_endpoint(BridgedAppliance *appliance);
    esp_err_t add_appliance(const StoredAppliance &stored, char *error, size_t error_len);
    void remove_orphaned_endpoints();

    /** Tell the SmartThings relay which devices to subscribe to (lock held). */
    void publish_smartthings_devices();

    esp_matter::node_t *m_node = nullptr;
    uint16_t m_aggregator_endpoint_id = 0xFFFF;
    std::vector<StoredAppliance> m_pending; // loaded at init, built at start
    std::vector<BridgedAppliance *> m_appliances;

    // Unbridged appliances are never deleted: esp-matter keeps the cluster
    // Instances it created for them, which point at their delegates.
    std::vector<BridgedAppliance *> m_retired;
};
