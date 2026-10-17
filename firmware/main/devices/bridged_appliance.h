#pragma once

#include <stdint.h>

#include <esp_matter.h>

#include "cJSON.h"

#include "bridge/appliance_store.h"

/**
 * A cloud appliance (HomeConnect or SmartThings) bridged onto one Matter endpoint.
 *
 * Threading: everything except the constructor and RequestRefresh() must be
 * called with the CHIP stack lock held (or on the Matter task). The lock is
 * the bridge's single lock: BridgeManager only adds or removes appliances
 * under it, so holding it also keeps the appliance alive.
 */
class BridgedAppliance
{
public:
    explicit BridgedAppliance(const StoredAppliance &stored) : m_stored(stored) {}
    virtual ~BridgedAppliance() = default;

    StoredAppliance &stored() { return m_stored; }
    const char *id() const { return m_stored.info.id; }
    const char *source() const { return m_stored.info.source; }
    bool is(const char *source, const char *id) const;
    uint16_t endpoint_id() const { return m_stored.endpoint_id; }
    void set_endpoint_id(uint16_t endpoint_id) { m_stored.endpoint_id = endpoint_id; }
    bool connected() const { return m_connected; }

    /** The esp_matter_bridge::device_t backing the endpoint. */
    void *bridge_device() const { return m_bridge_device; }
    void set_bridge_device(void *device) { m_bridge_device = device; }

    virtual uint32_t device_type_id() const = 0;

    /**
     * Add the device type's clusters to the bridged endpoint. Called from the
     * esp_matter_bridge device type callback, before delegates are initialised.
     * The base implementation fills in Bridged Device Basic Information.
     */
    virtual esp_err_t AddClusters(esp_matter::endpoint_t *ep);

    /** Called once the cluster delegates have been initialised. */
    virtual void OnEndpointReady() { Sync(); }

    /** Handle a HomeConnect STATUS / NOTIFY / EVENT from the event stream. */
    virtual void ApplyEvent(const char *event, const cJSON *data);

    /** Apply one HomeConnect { key, value } item to the cached state. */
    virtual void ApplyItem(const char *key, const cJSON *value) {}

    /** Apply one SmartThings capability attribute to the cached state. */
    virtual void ApplyAttribute(const char *capability, const char *attribute, const cJSON *value) {}

    /** Push the cached state into the Matter data model. */
    virtual void Sync() {}

    virtual void SetConnected(bool connected);

    /** Fetch a full snapshot from the appliance's cloud on the worker task. Any thread. */
    virtual void RequestRefresh() = 0;

    /** Describe the appliance for the web UI. */
    virtual void ToJson(cJSON *obj);

    /** A controller wrote an attribute on this endpoint (PRE_UPDATE). */
    virtual esp_err_t OnAttributeWrite(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val)
    {
        return ESP_OK;
    }

protected:
    StoredAppliance m_stored;
    bool m_connected = false;
    void *m_bridge_device = nullptr;
};

/** The final segment of a dotted HomeConnect key: "...OperationState.Run" -> "Run". */
const char *hc_key_suffix(const char *key);
