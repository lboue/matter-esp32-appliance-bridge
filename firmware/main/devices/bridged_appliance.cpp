#include "bridged_appliance.h"

#include <string.h>

#include <esp_matter_attribute_utils.h>

#include "esp_log.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

#define TAG "bridged_appliance"

const char *hc_key_suffix(const char *key)
{
    if (key == NULL) {
        return "";
    }
    const char *dot = strrchr(key, '.');
    return dot != NULL ? dot + 1 : key;
}

/** Copy into a fixed attribute buffer, truncating to the attribute's limit. */
static void bounded_copy(char *dst, size_t dst_size, const char *src, size_t limit)
{
    const size_t max = (limit + 1 < dst_size) ? limit + 1 : dst_size;
    strlcpy(dst, src, max);
}

bool BridgedAppliance::is(const char *source, const char *id) const
{
    return strcmp(m_stored.info.source, source) == 0 && strcmp(m_stored.info.id, id) == 0;
}

esp_err_t BridgedAppliance::AddClusters(endpoint_t *ep)
{
    cluster_t *basic = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    if (basic == nullptr) {
        return ESP_OK;
    }

    namespace bdbi = cluster::bridged_device_basic_information;
    char buf[bdbi::k_max_node_label_length + 1];

    // Controllers show NodeLabel as the device name.
    bounded_copy(buf, sizeof(buf), m_stored.info.name[0] ? m_stored.info.name : m_stored.info.type,
                 bdbi::k_max_node_label_length);
    bdbi::attribute::create_node_label(basic, buf, strlen(buf));

    bounded_copy(buf, sizeof(buf), m_stored.info.brand, bdbi::k_max_vendor_name_length);
    bdbi::attribute::create_vendor_name(basic, buf, strlen(buf));

    bounded_copy(buf, sizeof(buf), m_stored.info.model, bdbi::k_max_product_name_length);
    bdbi::attribute::create_product_name(basic, buf, strlen(buf));

    bounded_copy(buf, sizeof(buf), m_stored.info.id, bdbi::k_max_serial_number_length);
    bdbi::attribute::create_serial_number(basic, buf, strlen(buf));

    bounded_copy(buf, sizeof(buf), m_stored.info.id, bdbi::k_max_unique_id_length);
    bdbi::attribute::create_unique_id(basic, buf, strlen(buf));

    return ESP_OK;
}

void BridgedAppliance::ApplyEvent(const char *event, const cJSON *data)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "items");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items)
    {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
        if (cJSON_IsString(key)) {
            ApplyItem(key->valuestring, cJSON_GetObjectItemCaseSensitive(item, "value"));
        }
    }
    Sync();
}

void BridgedAppliance::SetConnected(bool connected)
{
    if (connected == m_connected) {
        return;
    }
    m_connected = connected;
    ESP_LOGI(TAG, "%s is %s", m_stored.info.id, connected ? "connected" : "disconnected");

    esp_matter_attr_val_t val = esp_matter_bool(connected);
    attribute::report(endpoint_id(), BridgedDeviceBasicInformation::Id,
                      BridgedDeviceBasicInformation::Attributes::Reachable::Id, &val);
}

void BridgedAppliance::ToJson(cJSON *obj)
{
    cJSON_AddStringToObject(obj, "source", m_stored.info.source);
    cJSON_AddStringToObject(obj, "id", m_stored.info.id);
    cJSON_AddStringToObject(obj, "name", m_stored.info.name);
    cJSON_AddStringToObject(obj, "brand", m_stored.info.brand);
    cJSON_AddStringToObject(obj, "model", m_stored.info.model);
    cJSON_AddStringToObject(obj, "type", m_stored.info.type);
    cJSON_AddNumberToObject(obj, "endpointId", m_stored.endpoint_id);
    cJSON_AddBoolToObject(obj, "connected", m_connected);
}
