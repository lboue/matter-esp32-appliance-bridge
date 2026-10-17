#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "esp_err.h"

#define APPLIANCE_SOURCE_HOMECONNECT "homeconnect"
#define APPLIANCE_SOURCE_SMARTTHINGS "smartthings"

#define APPLIANCE_SOURCE_LEN 16
#define APPLIANCE_ID_LEN 48 // HomeConnect haIds (~30 chars) and SmartThings UUIDs (36)
#define APPLIANCE_NAME_LEN 64
#define APPLIANCE_SHORT_LEN 40

/** Who an appliance belongs to and what it is, whichever cloud it comes from. */
struct ApplianceInfo {
    char source[APPLIANCE_SOURCE_LEN]; // APPLIANCE_SOURCE_*
    char id[APPLIANCE_ID_LEN];         // HomeConnect haId or SmartThings deviceId
    char name[APPLIANCE_NAME_LEN];
    char brand[APPLIANCE_SHORT_LEN];
    char model[APPLIANCE_SHORT_LEN];
    char type[APPLIANCE_SHORT_LEN]; // HomeConnect type ("Dishwasher") or "Washer"
};

/** One HomeConnect program, exposed as one Matter mode. */
struct HcProgram {
    std::string key;  // e.g. "Dishcare.Dishwasher.Program.Eco50"
    std::string name; // display name from HomeConnect, may be empty
};

/**
 * What the bridge remembers about a bridged appliance across reboots.
 *
 * The endpoint ID must be stable or controllers lose track of the device. The
 * program list (HomeConnect only) is kept so the mode cluster has its
 * supported modes at boot, before HomeConnect has been reached; the index in
 * `programs` is the mode value, so programs are only ever appended.
 */
struct StoredAppliance {
    ApplianceInfo info = {};
    uint16_t endpoint_id = 0xFFFF;
    std::vector<HcProgram> programs;
};

esp_err_t appliance_store_load(std::vector<StoredAppliance> &out);
esp_err_t appliance_store_save(const std::vector<const StoredAppliance *> &appliances);
