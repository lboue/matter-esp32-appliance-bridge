#include "appliance_store.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

#define TAG "appliance_store"

// Not "bridge": esp_matter_bridge owns that namespace and erases it wholesale.
#define NVS_NAMESPACE "appliances"
#define NVS_KEY_APPLIANCES "appliances"

static void get_string(const cJSON *obj, const char *name, char *out, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(item)) {
        strlcpy(out, item->valuestring, len);
    }
}

esp_err_t appliance_store_load(std::vector<StoredAppliance> &out)
{
    out.clear();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    size_t size = 0;
    err = nvs_get_blob(handle, NVS_KEY_APPLIANCES, NULL, &size);
    if (err != ESP_OK || size == 0) {
        nvs_close(handle);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    char *json = (char *)malloc(size + 1);
    if (json == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(handle, NVS_KEY_APPLIANCES, json, &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(json);
        return err;
    }
    json[size] = '\0';

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Stored appliance list is corrupt; ignoring it");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, root)
    {
        StoredAppliance stored;
        // Records written before SmartThings support have no source and use
        // HomeConnect's field names; they are all HomeConnect appliances.
        strlcpy(stored.info.source, APPLIANCE_SOURCE_HOMECONNECT, sizeof(stored.info.source));
        get_string(item, "source", stored.info.source, sizeof(stored.info.source));
        get_string(item, "haId", stored.info.id, sizeof(stored.info.id));
        get_string(item, "id", stored.info.id, sizeof(stored.info.id));
        get_string(item, "name", stored.info.name, sizeof(stored.info.name));
        get_string(item, "brand", stored.info.brand, sizeof(stored.info.brand));
        get_string(item, "vib", stored.info.model, sizeof(stored.info.model));
        get_string(item, "model", stored.info.model, sizeof(stored.info.model));
        get_string(item, "type", stored.info.type, sizeof(stored.info.type));

        const cJSON *ep = cJSON_GetObjectItemCaseSensitive(item, "ep");
        if (cJSON_IsNumber(ep)) {
            stored.endpoint_id = (uint16_t)ep->valueint;
        }

        const cJSON *program = NULL;
        cJSON_ArrayForEach(program, cJSON_GetObjectItemCaseSensitive(item, "programs"))
        {
            const cJSON *key = cJSON_GetObjectItemCaseSensitive(program, "k");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(program, "n");
            if (cJSON_IsString(key)) {
                stored.programs.push_back({key->valuestring, cJSON_IsString(name) ? name->valuestring : ""});
            }
        }

        if (stored.info.id[0] != '\0') {
            out.push_back(std::move(stored));
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %u bridged appliance(s)", (unsigned)out.size());
    return ESP_OK;
}

esp_err_t appliance_store_save(const std::vector<const StoredAppliance *> &appliances)
{
    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (const StoredAppliance *stored : appliances) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "source", stored->info.source);
        cJSON_AddStringToObject(item, "id", stored->info.id);
        cJSON_AddStringToObject(item, "name", stored->info.name);
        cJSON_AddStringToObject(item, "brand", stored->info.brand);
        cJSON_AddStringToObject(item, "model", stored->info.model);
        cJSON_AddStringToObject(item, "type", stored->info.type);
        cJSON_AddNumberToObject(item, "ep", stored->endpoint_id);

        cJSON *programs = cJSON_AddArrayToObject(item, "programs");
        for (const HcProgram &program : stored->programs) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "k", program.key.c_str());
            if (!program.name.empty()) {
                cJSON_AddStringToObject(p, "n", program.name.c_str());
            }
            cJSON_AddItemToArray(programs, p);
        }
        cJSON_AddItemToArray(root, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_KEY_APPLIANCES, json, strlen(json));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    free(json);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save appliance list: %s", esp_err_to_name(err));
    }
    return err;
}
