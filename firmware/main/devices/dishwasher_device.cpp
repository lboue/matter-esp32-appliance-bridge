#include "dishwasher_device.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <app/clusters/dishwasher-alarm-server/dishwasher-alarm-server.h>
#include <esp_matter_attribute_utils.h>
#include <esp_matter_endpoint.h>

#include "esp_log.h"

#include "bridge/bridge_manager.h"
#include "homeconnect/hc_client.h"
#include "cloud_worker.h"
#include "web_server.h"

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::OperationalState;
using namespace esp_matter;
using chip::Protocols::InteractionModel::Status;
using ModeTagStructType = chip::app::Clusters::detail::Structs::ModeTagStruct::Type;

#define TAG "dishwasher"

#define PATH_LEN 192
#define BODY_LEN 192

// HomeConnect keys
#define KEY_OPERATION_STATE "BSH.Common.Status.OperationState"
#define KEY_DOOR_STATE "BSH.Common.Status.DoorState"
#define KEY_REMOTE_START_ALLOWED "BSH.Common.Status.RemoteControlStartAllowed"
#define KEY_REMOTE_CONTROL_ACTIVE "BSH.Common.Status.RemoteControlActive"
#define KEY_POWER_STATE "BSH.Common.Setting.PowerState"
#define KEY_REMAINING_TIME "BSH.Common.Option.RemainingProgramTime"
#define KEY_PROGRESS "BSH.Common.Option.ProgramProgress"
#define KEY_ACTIVE_PROGRAM "BSH.Common.Root.ActiveProgram"
#define KEY_SELECTED_PROGRAM "BSH.Common.Root.SelectedProgram"
#define KEY_SALT_NEARLY_EMPTY "Dishcare.Dishwasher.Event.SaltNearlyEmpty"
#define KEY_RINSE_AID_NEARLY_EMPTY "Dishcare.Dishwasher.Event.RinseAidNearlyEmpty"
#define KEY_CMD_PAUSE "BSH.Common.Command.PauseProgram"
#define KEY_CMD_RESUME "BSH.Common.Command.ResumeProgram"

namespace {

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

const char *string_value(const cJSON *value)
{
    return cJSON_IsString(value) ? value->valuestring : "";
}

/** -1 if the value is not a boolean. */
int bool_value(const cJSON *value)
{
    if (cJSON_IsBool(value)) {
        return cJSON_IsTrue(value) ? 1 : 0;
    }
    return -1;
}

DishwasherDevice::HcOperationState parse_operation_state(const char *value)
{
    using S = DishwasherDevice::HcOperationState;
    const char *s = hc_key_suffix(value);
    if (strcmp(s, "Inactive") == 0) return S::Inactive;
    if (strcmp(s, "Ready") == 0) return S::Ready;
    if (strcmp(s, "DelayedStart") == 0) return S::DelayedStart;
    if (strcmp(s, "Run") == 0) return S::Run;
    if (strcmp(s, "Pause") == 0) return S::Pause;
    if (strcmp(s, "ActionRequired") == 0) return S::ActionRequired;
    if (strcmp(s, "Finished") == 0) return S::Finished;
    if (strcmp(s, "Error") == 0) return S::Error;
    if (strcmp(s, "Aborting") == 0) return S::Aborting;
    return S::Unknown;
}

const char *operation_state_name(DishwasherDevice::HcOperationState state)
{
    using S = DishwasherDevice::HcOperationState;
    switch (state) {
    case S::Inactive: return "Inactive";
    case S::Ready: return "Ready";
    case S::DelayedStart: return "DelayedStart";
    case S::Run: return "Run";
    case S::Pause: return "Pause";
    case S::ActionRequired: return "ActionRequired";
    case S::Finished: return "Finished";
    case S::Error: return "Error";
    case S::Aborting: return "Aborting";
    case S::Unknown: break;
    }
    return "Unknown";
}

/** Dishcare.Dishwasher.EnumType.EventPresentState.Present -> true. */
bool event_present(const cJSON *value)
{
    return strcmp(hc_key_suffix(string_value(value)), "Present") == 0;
}

bool contains_ignore_case(const char *haystack, const char *needle)
{
    const size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Matter mode tags for a HomeConnect program, guessed from its key. Program
 * keys are stable across languages ("Dishcare.Dishwasher.Program.Eco50"),
 * unlike names. Returns the number of tags written.
 */
size_t tags_for_program(const char *key, DishwasherMode::ModeTag *tags, size_t max)
{
    const char *p = hc_key_suffix(key);
    size_t n = 0;
    auto add = [&](DishwasherMode::ModeTag tag) {
        if (n < max) {
            tags[n++] = tag;
        }
    };

    if (contains_ignore_case(p, "eco")) {
        add(DishwasherMode::ModeTag::kNormal);
        add(DishwasherMode::ModeTag::kLowEnergy);
    } else if (contains_ignore_case(p, "intensiv") || contains_ignore_case(p, "power") ||
               contains_ignore_case(p, "machinecare") || contains_ignore_case(p, "hygiene")) {
        add(DishwasherMode::ModeTag::kHeavy);
        add(DishwasherMode::ModeTag::kMax);
    } else if (contains_ignore_case(p, "quick") || contains_ignore_case(p, "kurz") ||
               contains_ignore_case(p, "express") || contains_ignore_case(p, "speed") ||
               contains_ignore_case(p, "short")) {
        add(DishwasherMode::ModeTag::kLight);
        add(DishwasherMode::ModeTag::kQuick);
    } else if (contains_ignore_case(p, "night") || contains_ignore_case(p, "silence") ||
               contains_ignore_case(p, "quiet")) {
        add(DishwasherMode::ModeTag::kNight);
        add(DishwasherMode::ModeTag::kQuiet);
    } else if (contains_ignore_case(p, "glas") || contains_ignore_case(p, "delicate")) {
        add(DishwasherMode::ModeTag::kLight);
    } else if (contains_ignore_case(p, "prerinse") || contains_ignore_case(p, "rinse")) {
        add(DishwasherMode::ModeTag::kMin);
    } else if (contains_ignore_case(p, "auto")) {
        add(DishwasherMode::ModeTag::kAuto);
    } else {
        add(DishwasherMode::ModeTag::kNormal);
    }
    return n;
}

/** A readable label from a program key: "...Program.Intensiv70" -> "Intensiv70". */
void label_for_program(const HcProgram &program, char *out, size_t len)
{
    if (!program.name.empty()) {
        strlcpy(out, program.name.c_str(), len);
    } else {
        strlcpy(out, hc_key_suffix(program.key.c_str()), len);
    }
}

// ---------------------------------------------------------------------------
// Worker jobs. Each copies what it needs, does the HTTPS call(s) off the CHIP
// stack lock, then re-finds the appliance under the lock to apply the result
// (it may have been unbridged in the meantime).
// ---------------------------------------------------------------------------

enum class CommandKind : uint8_t { SelectProgram, Start, Stop, Pause, Resume, Power };

struct CommandJob {
    char ha_id[HC_HAID_LEN];
    CommandKind kind;
    char key[HC_PROGRAM_KEY_LEN];
    bool on;
};

const char *command_name(CommandKind kind)
{
    switch (kind) {
    case CommandKind::SelectProgram: return "select program";
    case CommandKind::Start: return "start";
    case CommandKind::Stop: return "stop";
    case CommandKind::Pause: return "pause";
    case CommandKind::Resume: return "resume";
    case CommandKind::Power: return "power";
    }
    return "command";
}

void notify_ui(const char *ha_id)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"type\":\"appliance\",\"haId\":\"%s\"}", ha_id);
    web_server_notify_ws_event(msg);
}

void run_command_job(void *arg)
{
    CommandJob *job = static_cast<CommandJob *>(arg);
    char path[PATH_LEN];
    char body[BODY_LEN];
    esp_err_t err = ESP_FAIL;

    switch (job->kind) {
    case CommandKind::SelectProgram:
        snprintf(path, sizeof(path), "/api/homeappliances/%s/programs/selected", job->ha_id);
        snprintf(body, sizeof(body), "{\"data\":{\"key\":\"%s\"}}", job->key);
        err = hc_client_put(path, body);
        break;
    case CommandKind::Start:
        snprintf(path, sizeof(path), "/api/homeappliances/%s/programs/active", job->ha_id);
        snprintf(body, sizeof(body), "{\"data\":{\"key\":\"%s\"}}", job->key);
        err = hc_client_put(path, body);
        break;
    case CommandKind::Stop:
        snprintf(path, sizeof(path), "/api/homeappliances/%s/programs/active", job->ha_id);
        err = hc_client_delete(path);
        break;
    case CommandKind::Pause:
    case CommandKind::Resume: {
        const char *command = job->kind == CommandKind::Pause ? KEY_CMD_PAUSE : KEY_CMD_RESUME;
        snprintf(path, sizeof(path), "/api/homeappliances/%s/commands/%s", job->ha_id, command);
        snprintf(body, sizeof(body), "{\"data\":{\"key\":\"%s\",\"value\":true}}", command);
        err = hc_client_put(path, body);
        break;
    }
    case CommandKind::Power:
        snprintf(path, sizeof(path), "/api/homeappliances/%s/settings/" KEY_POWER_STATE, job->ha_id);
        snprintf(body, sizeof(body), "{\"data\":{\"key\":\"" KEY_POWER_STATE "\",\"value\":\"BSH.Common.EnumType.PowerState.%s\"}}",
                 job->on ? "On" : "Off");
        err = hc_client_put(path, body);
        break;
    }

    char error[128] = "";
    if (err != ESP_OK) {
        char detail[96];
        hc_client_last_error(detail, sizeof(detail));
        snprintf(error, sizeof(error), "%s failed: %s", command_name(job->kind), detail);
        ESP_LOGW(TAG, "%s: %s", job->ha_id, error);
    } else {
        ESP_LOGI(TAG, "%s: %s accepted", job->ha_id, command_name(job->kind));
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    BridgedAppliance *appliance = BridgeManager::instance().find(APPLIANCE_SOURCE_HOMECONNECT, job->ha_id);
    if (appliance != nullptr && appliance->device_type_id() == ESP_MATTER_DISH_WASHER_DEVICE_TYPE_ID) {
        auto *dishwasher = static_cast<DishwasherDevice *>(appliance);
        dishwasher->SetLastCommandError(error);
        // On failure, put CurrentMode / OnOff back to what the appliance has.
        dishwasher->Sync();
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    notify_ui(job->ha_id);
    free(job);
}

/** GET a path and return the parsed body's "data" object, or NULL. */
cJSON *get_data(const char *ha_id, const char *suffix, cJSON **root, esp_err_t *err_out = nullptr)
{
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "/api/homeappliances/%s%s", ha_id, suffix);
    esp_err_t err = hc_client_get(path, root);
    if (err_out != nullptr) {
        *err_out = err;
    }
    return err == ESP_OK ? cJSON_GetObjectItemCaseSensitive(*root, "data") : nullptr;
}

void apply_items(DishwasherDevice *device, const cJSON *list)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, list)
    {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
        if (cJSON_IsString(key)) {
            device->ApplyItem(key->valuestring, cJSON_GetObjectItemCaseSensitive(item, "value"));
        }
    }
}

void run_refresh_job(void *arg)
{
    char *ha_id = static_cast<char *>(arg);

    cJSON *info_root = NULL, *status_root = NULL, *settings_root = NULL, *programs_root = NULL;
    cJSON *active_root = NULL, *selected_root = NULL, *commands_root = NULL;

    const cJSON *info = get_data(ha_id, "", &info_root);
    const bool connected = info != nullptr && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(info, "connected"));

    const cJSON *status = nullptr, *settings = nullptr, *programs = nullptr;
    const cJSON *active = nullptr, *selected = nullptr, *commands = nullptr;
    esp_err_t active_err = ESP_FAIL, selected_err = ESP_FAIL;
    if (connected) {
        status = get_data(ha_id, "/status", &status_root);
        settings = get_data(ha_id, "/settings", &settings_root);
        programs = get_data(ha_id, "/programs/available", &programs_root);
        active = get_data(ha_id, "/programs/active", &active_root, &active_err);
        selected = get_data(ha_id, "/programs/selected", &selected_root, &selected_err);
        commands = get_data(ha_id, "/commands", &commands_root);
    }

    bool save = false;
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    BridgedAppliance *appliance = BridgeManager::instance().find(APPLIANCE_SOURCE_HOMECONNECT, ha_id);
    if (appliance != nullptr && appliance->device_type_id() == ESP_MATTER_DISH_WASHER_DEVICE_TYPE_ID) {
        auto *device = static_cast<DishwasherDevice *>(appliance);

        if (info != nullptr) {
            hc_appliance_t fresh;
            hc_client_parse_appliance(info, &fresh);
            ApplianceInfo &stored = device->stored().info;
            if (fresh.name[0] != '\0' && strcmp(fresh.name, stored.name) != 0) {
                strlcpy(stored.name, fresh.name, sizeof(stored.name));
                save = true;
            }
            device->SetConnected(connected);
        }

        if (connected) {
            apply_items(device, cJSON_GetObjectItemCaseSensitive(status, "status"));
            apply_items(device, cJSON_GetObjectItemCaseSensitive(settings, "settings"));
            if (programs != nullptr && device->MergePrograms(cJSON_GetObjectItemCaseSensitive(programs, "programs"))) {
                save = true;
            }

            // 404 means "no program", anything else means "don't know".
            if (active != nullptr) {
                device->SetActiveProgram(string_value(cJSON_GetObjectItemCaseSensitive(active, "key")));
                apply_items(device, cJSON_GetObjectItemCaseSensitive(active, "options"));
            } else if (active_err == ESP_ERR_NOT_FOUND) {
                device->SetActiveProgram("");
            }
            if (selected != nullptr) {
                device->SetSelectedProgram(string_value(cJSON_GetObjectItemCaseSensitive(selected, "key")));
            } else if (selected_err == ESP_ERR_NOT_FOUND) {
                device->SetSelectedProgram("");
            }

            bool can_pause = false, can_resume = false;
            const cJSON *command = NULL;
            cJSON_ArrayForEach(command, cJSON_GetObjectItemCaseSensitive(commands, "commands"))
            {
                const char *key = string_value(cJSON_GetObjectItemCaseSensitive(command, "key"));
                can_pause |= strcmp(key, KEY_CMD_PAUSE) == 0;
                can_resume |= strcmp(key, KEY_CMD_RESUME) == 0;
            }
            device->SetCommandSupport(can_pause, can_resume);
        }

        device->Sync();
        if (save) {
            BridgeManager::instance().save();
        }
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    cJSON_Delete(info_root);
    cJSON_Delete(status_root);
    cJSON_Delete(settings_root);
    cJSON_Delete(programs_root);
    cJSON_Delete(active_root);
    cJSON_Delete(selected_root);
    cJSON_Delete(commands_root);

    ESP_LOGI(TAG, "Refreshed %s (%s)", ha_id, connected ? "connected" : "offline");
    notify_ui(ha_id);
    free(ha_id);
}

esp_err_t post_command(const char *ha_id, CommandKind kind, const char *key = "", bool on = false)
{
    CommandJob *job = static_cast<CommandJob *>(calloc(1, sizeof(CommandJob)));
    if (job == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(job->ha_id, ha_id, sizeof(job->ha_id));
    strlcpy(job->key, key, sizeof(job->key));
    job->kind = kind;
    job->on = on;
    esp_err_t err = cloud_worker_post(run_command_job, job);
    if (err != ESP_OK) {
        free(job);
    }
    return err;
}

} // namespace

// ---------------------------------------------------------------------------
// Operational State delegate
// ---------------------------------------------------------------------------

namespace {
const GenericOperationalState kOperationalStates[] = {
    GenericOperationalState(to_underlying(OperationalStateEnum::kStopped)),
    GenericOperationalState(to_underlying(OperationalStateEnum::kRunning)),
    GenericOperationalState(to_underlying(OperationalStateEnum::kPaused)),
    GenericOperationalState(to_underlying(OperationalStateEnum::kError)),
};
} // namespace

DataModel::Nullable<uint32_t> DishwasherOperationalStateDelegate::GetCountdownTime()
{
    return m_device.CountdownTime();
}

CHIP_ERROR DishwasherOperationalStateDelegate::GetOperationalStateAtIndex(size_t index,
                                                                         GenericOperationalState &operationalState)
{
    if (index >= sizeof(kOperationalStates) / sizeof(kOperationalStates[0])) {
        return CHIP_ERROR_NOT_FOUND;
    }
    operationalState = kOperationalStates[index];
    return CHIP_NO_ERROR;
}

CHIP_ERROR DishwasherOperationalStateDelegate::GetOperationalPhaseAtIndex(size_t index, MutableCharSpan &operationalPhase)
{
    // HomeConnect does not report wash phases for dishwashers, so PhaseList
    // and CurrentPhase are null.
    return CHIP_ERROR_NOT_FOUND;
}

void DishwasherOperationalStateDelegate::HandlePauseStateCallback(GenericOperationalError &err)
{
    m_device.HandlePause(err);
}

void DishwasherOperationalStateDelegate::HandleResumeStateCallback(GenericOperationalError &err)
{
    m_device.HandleResume(err);
}

void DishwasherOperationalStateDelegate::HandleStartStateCallback(GenericOperationalError &err)
{
    m_device.HandleStart(err);
}

void DishwasherOperationalStateDelegate::HandleStopStateCallback(GenericOperationalError &err)
{
    m_device.HandleStop(err);
}

// ---------------------------------------------------------------------------
// Dishwasher Mode delegate
// ---------------------------------------------------------------------------

void DishwasherModeDelegate::HandleChangeToMode(uint8_t new_mode, ModeBase::Commands::ChangeToModeResponse::Type &response)
{
    m_device.HandleChangeToMode(new_mode, response);
}

CHIP_ERROR DishwasherModeDelegate::GetModeLabelByIndex(uint8_t mode_index, MutableCharSpan &label)
{
    if (mode_index >= m_device.ModeCount()) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    char buf[65];
    label_for_program(m_device.ProgramAt(mode_index), buf, sizeof(buf));
    return CopyCharSpanToMutableCharSpan(CharSpan::fromCharString(buf), label);
}

CHIP_ERROR DishwasherModeDelegate::GetModeValueByIndex(uint8_t mode_index, uint8_t &value)
{
    if (mode_index >= m_device.ModeCount()) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    // Programs are only ever appended, so the index is a stable mode value.
    value = mode_index;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DishwasherModeDelegate::GetModeTagsByIndex(uint8_t mode_index, DataModel::List<ModeTagStructType> &tags)
{
    if (mode_index >= m_device.ModeCount()) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }

    DishwasherMode::ModeTag program_tags[4];
    size_t count = tags_for_program(m_device.ProgramAt(mode_index).key.c_str(), program_tags, 4);

    // The spec requires at least one mode to carry the Normal tag. If no
    // program looks like an Eco/normal one, give it to the first mode.
    if (mode_index == 0) {
        bool any_normal = false;
        for (size_t i = 0; i < m_device.ModeCount() && !any_normal; i++) {
            DishwasherMode::ModeTag t[4];
            const size_t n = tags_for_program(m_device.ProgramAt(i).key.c_str(), t, 4);
            for (size_t j = 0; j < n; j++) {
                any_normal |= t[j] == DishwasherMode::ModeTag::kNormal;
            }
        }
        if (!any_normal && count < 4) {
            program_tags[count++] = DishwasherMode::ModeTag::kNormal;
        }
    }

    if (tags.size() < count) {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    for (size_t i = 0; i < count; i++) {
        tags[i] = ModeTagStructType{.value = to_underlying(program_tags[i])};
    }
    tags.reduce_size(count);
    return CHIP_NO_ERROR;
}

// ---------------------------------------------------------------------------
// Dishwasher device
// ---------------------------------------------------------------------------

DishwasherDevice::DishwasherDevice(const StoredAppliance &stored) :
    BridgedAppliance(stored), m_operational_state_delegate(*this), m_mode_delegate(*this)
{}

uint32_t DishwasherDevice::device_type_id() const
{
    return ESP_MATTER_DISH_WASHER_DEVICE_TYPE_ID;
}

esp_err_t DishwasherDevice::AddClusters(endpoint_t *ep)
{
    BridgedAppliance::AddClusters(ep);

    endpoint::dish_washer::config_t dishwasher_config;
    dishwasher_config.operational_state.delegate = &m_operational_state_delegate;
    esp_err_t err = endpoint::dish_washer::add(ep, &dishwasher_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add dishwasher clusters: %s", esp_err_to_name(err));
        return err;
    }

    cluster_t *opstate = cluster::get(ep, OperationalState::Id);
    if (opstate != nullptr) {
        cluster::operational_state::attribute::create_countdown_time(opstate, nullable<uint32_t>());
        cluster::operational_state::command::create_start(opstate);
        cluster::operational_state::command::create_stop(opstate);
        cluster::operational_state::command::create_pause(opstate);
        cluster::operational_state::command::create_resume(opstate);
        cluster::operational_state::command::create_operational_command_response(opstate);
    }

    // Mode list is served by the delegate from the HomeConnect program list.
    cluster::dish_washer_mode::config_t mode_config;
    mode_config.delegate = &m_mode_delegate;
    mode_config.current_mode = 0;
    cluster_t *mode = cluster::dish_washer_mode::create(ep, &mode_config, CLUSTER_FLAG_SERVER);
    if (mode != nullptr) {
        cluster::mode_base::attribute::create_supported_modes(mode, NULL, 0, 0);
        cluster::mode_base::command::create_change_to_mode(mode);
    }

    // Power, with DeadFront: while off, the other clusters report defaults.
    cluster::on_off::config_t on_off_config;
    cluster_t *on_off = cluster::on_off::create(ep, &on_off_config, CLUSTER_FLAG_SERVER);
    if (on_off != nullptr) {
        cluster::on_off::feature::dead_front_behavior::add(on_off);
        cluster::on_off::command::create_on(on_off);
        cluster::on_off::command::create_toggle(on_off);
    }

    // esp-matter's dish_washer_alarm::create does not add the mandatory
    // Mask/State/Supported attributes, which the alarm server reads through
    // the attribute store, so add them here.
    cluster::dish_washer_alarm::config_t alarm_config;
    alarm_config.delegate = &m_alarm_delegate;
    cluster_t *alarm = cluster::dish_washer_alarm::create(ep, &alarm_config, CLUSTER_FLAG_SERVER);
    if (alarm != nullptr) {
        attribute::create(alarm, DishwasherAlarm::Attributes::Mask::Id, ATTRIBUTE_FLAG_NONE, esp_matter_bitmap32(0));
        attribute::create(alarm, DishwasherAlarm::Attributes::State::Id, ATTRIBUTE_FLAG_NONE, esp_matter_bitmap32(0));
        attribute::create(alarm, DishwasherAlarm::Attributes::Supported::Id, ATTRIBUTE_FLAG_NONE,
                          esp_matter_bitmap32(0));
    }

    return ESP_OK;
}

void DishwasherDevice::OnEndpointReady()
{
    m_endpoint_ready = true;

    const BitMask<DishwasherAlarm::AlarmMap> door(DishwasherAlarm::AlarmMap::kDoorError);
    auto &alarm_server = DishwasherAlarm::DishwasherAlarmServer::Instance();
    alarm_server.SetSupportedValue(endpoint_id(), door);
    alarm_server.SetMaskValue(endpoint_id(), door);

    Sync();
}

void DishwasherDevice::ApplyItem(const char *key, const cJSON *value)
{
    if (strcmp(key, KEY_OPERATION_STATE) == 0) {
        m_operation_state = parse_operation_state(string_value(value));
    } else if (strcmp(key, KEY_DOOR_STATE) == 0) {
        m_door_state = hc_key_suffix(string_value(value));
    } else if (strcmp(key, KEY_POWER_STATE) == 0) {
        m_power_state = hc_key_suffix(string_value(value));
    } else if (strcmp(key, KEY_REMOTE_START_ALLOWED) == 0) {
        m_remote_start_allowed = bool_value(value);
    } else if (strcmp(key, KEY_REMOTE_CONTROL_ACTIVE) == 0) {
        m_remote_control_active = bool_value(value);
    } else if (strcmp(key, KEY_REMAINING_TIME) == 0) {
        m_remaining_s = cJSON_IsNumber(value) ? value->valueint : -1;
    } else if (strcmp(key, KEY_PROGRESS) == 0) {
        m_progress_pct = cJSON_IsNumber(value) ? value->valueint : -1;
    } else if (strcmp(key, KEY_ACTIVE_PROGRAM) == 0) {
        SetActiveProgram(string_value(value));
    } else if (strcmp(key, KEY_SELECTED_PROGRAM) == 0) {
        SetSelectedProgram(string_value(value));
    } else if (strcmp(key, KEY_SALT_NEARLY_EMPTY) == 0) {
        m_salt_low = event_present(value);
    } else if (strcmp(key, KEY_RINSE_AID_NEARLY_EMPTY) == 0) {
        m_rinse_aid_low = event_present(value);
    } else {
        ESP_LOGD(TAG, "%s: unmapped %s", id(), key);
    }
}

void DishwasherDevice::SetActiveProgram(const char *key)
{
    m_active_program = key != nullptr ? key : "";
    if (m_active_program.empty()) {
        m_remaining_s = -1;
        m_progress_pct = -1;
    }
}

void DishwasherDevice::SetSelectedProgram(const char *key)
{
    m_selected_program = key != nullptr ? key : "";
}

void DishwasherDevice::SetCommandSupport(bool pause, bool resume)
{
    m_can_pause = pause;
    m_can_resume = resume;
}

void DishwasherDevice::SetLastCommandError(const char *message)
{
    m_last_command_error = message != nullptr ? message : "";
}

bool DishwasherDevice::MergePrograms(const cJSON *programs)
{
    bool grew = false;
    const cJSON *program = NULL;
    cJSON_ArrayForEach(program, programs)
    {
        const char *key = string_value(cJSON_GetObjectItemCaseSensitive(program, "key"));
        const char *name = string_value(cJSON_GetObjectItemCaseSensitive(program, "name"));
        if (key[0] == '\0' || strlen(key) >= HC_PROGRAM_KEY_LEN) {
            continue;
        }
        const int existing = ModeForProgram(key);
        if (existing >= 0) {
            // Pick up a display name that arrived after the program was stored.
            HcProgram &stored = m_stored.programs[existing];
            if (name[0] != '\0' && stored.name != name) {
                stored.name = name;
                grew = true;
            }
            continue;
        }
        if (m_stored.programs.size() >= HC_MAX_PROGRAMS) {
            ESP_LOGW(TAG, "%s: program list full; ignoring %s", id(), key);
            continue;
        }
        m_stored.programs.push_back({key, name});
        grew = true;
    }

    if (grew && m_endpoint_ready && m_mode_delegate.instance() != nullptr) {
        m_mode_delegate.instance()->ReportSupportedModesChange();
    }
    return grew;
}

int DishwasherDevice::ModeForProgram(const std::string &key) const
{
    for (size_t i = 0; i < m_stored.programs.size(); i++) {
        if (m_stored.programs[i].key == key) {
            return (int)i;
        }
    }
    return -1;
}

DataModel::Nullable<uint32_t> DishwasherDevice::CountdownTime() const
{
    const bool in_program = m_operation_state == HcOperationState::Run ||
                            m_operation_state == HcOperationState::Pause ||
                            m_operation_state == HcOperationState::DelayedStart ||
                            m_operation_state == HcOperationState::ActionRequired;
    if (!in_program || m_remaining_s < 0) {
        return DataModel::NullNullable;
    }
    return DataModel::MakeNullable((uint32_t)m_remaining_s);
}

// ---- Sync: cached state -> Matter ----

void DishwasherDevice::Sync()
{
    if (!m_endpoint_ready) {
        return;
    }
    SyncOperationalState();
    SyncMode();
    SyncOnOff();
    SyncAlarm();
}

void DishwasherDevice::SyncOperationalState()
{
    OperationalState::Instance *instance = m_operational_state_delegate.instance();
    if (instance == nullptr || m_operation_state == HcOperationState::Unknown) {
        return;
    }

    if (m_operation_state == HcOperationState::Error) {
        if (!m_error_reported) {
            Structs::ErrorStateStruct::Type error;
            error.errorStateID = to_underlying(ErrorStateEnum::kUnableToCompleteOperation);
            instance->OnOperationalErrorDetected(error);
            m_error_reported = true;
        }
        instance->UpdateCountdownTimeFromDelegate();
        return;
    }
    m_error_reported = false;

    OperationalStateEnum state;
    switch (m_operation_state) {
    case HcOperationState::Run:
        state = OperationalStateEnum::kRunning;
        break;
    case HcOperationState::Pause:
    case HcOperationState::ActionRequired:
        state = OperationalStateEnum::kPaused;
        break;
    default:
        // Inactive, Ready, DelayedStart, Finished, Aborting
        state = OperationalStateEnum::kStopped;
        break;
    }

    const bool running = state == OperationalStateEnum::kRunning || state == OperationalStateEnum::kPaused;
    if (m_was_running && m_operation_state == HcOperationState::Finished) {
        // Emits OperationCompletion and moves the cluster to Stopped.
        instance->OnOperationCompletionDetected(to_underlying(ErrorStateEnum::kNoError));
    }
    m_was_running = running;

    CHIP_ERROR err = instance->SetOperationalState(to_underlying(state));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "%s: SetOperationalState failed: %" CHIP_ERROR_FORMAT, id(), err.Format());
    }
    instance->UpdateCountdownTimeFromDelegate();
}

void DishwasherDevice::SyncMode()
{
    ModeBase::Instance *instance = m_mode_delegate.instance();
    if (instance == nullptr) {
        return;
    }
    const std::string &program = !m_active_program.empty() ? m_active_program : m_selected_program;
    const int mode = ModeForProgram(program);
    if (mode >= 0 && instance->GetCurrentMode() != mode) {
        instance->UpdateCurrentMode((uint8_t)mode);
    }
}

void DishwasherDevice::SyncOnOff()
{
    if (m_power_state.empty()) {
        return;
    }
    const int on = m_power_state == "On" ? 1 : 0;
    if (on == m_reported_on_off) {
        return;
    }
    m_reported_on_off = on;
    // report() rather than update(): this change came from the appliance, so it
    // must not loop back through app_attribute_update_cb to HomeConnect.
    esp_matter_attr_val_t val = esp_matter_bool(on == 1);
    attribute::report(endpoint_id(), OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

void DishwasherDevice::SyncAlarm()
{
    // A door opened mid-program is the only alarm HomeConnect lets us infer.
    const bool in_program =
        m_operation_state == HcOperationState::Run || m_operation_state == HcOperationState::Pause;
    BitMask<DishwasherAlarm::AlarmMap> state;
    if (in_program && m_door_state == "Open") {
        state.Set(DishwasherAlarm::AlarmMap::kDoorError);
    }
    DishwasherAlarm::DishwasherAlarmServer::Instance().SetStateValue(endpoint_id(), state, false);
}

// ---- Matter commands (Matter task, lock held) ----

void DishwasherDevice::HandleStart(GenericOperationalError &err)
{
    if (!m_connected || m_remote_start_allowed == 0) {
        // The user has to enable Remote Start on the appliance itself.
        err.Set(to_underlying(ErrorStateEnum::kUnableToStartOrResume));
        return;
    }

    std::string program;
    ModeBase::Instance *mode = m_mode_delegate.instance();
    if (mode != nullptr && mode->GetCurrentMode() < m_stored.programs.size()) {
        program = m_stored.programs[mode->GetCurrentMode()].key;
    } else {
        program = m_selected_program;
    }
    if (program.empty()) {
        err.Set(to_underlying(ErrorStateEnum::kCommandInvalidInState));
        return;
    }

    if (post_command(id(), CommandKind::Start, program.c_str()) != ESP_OK) {
        err.Set(to_underlying(ErrorStateEnum::kUnableToStartOrResume));
        return;
    }
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

void DishwasherDevice::HandleStop(GenericOperationalError &err)
{
    if (!m_connected || m_remote_control_active == 0) {
        err.Set(to_underlying(ErrorStateEnum::kCommandInvalidInState));
        return;
    }
    if (post_command(id(), CommandKind::Stop) != ESP_OK) {
        err.Set(to_underlying(ErrorStateEnum::kCommandInvalidInState));
        return;
    }
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

void DishwasherDevice::HandlePause(GenericOperationalError &err)
{
    if (!m_connected || !m_can_pause || post_command(id(), CommandKind::Pause) != ESP_OK) {
        err.Set(to_underlying(ErrorStateEnum::kCommandInvalidInState));
        return;
    }
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

void DishwasherDevice::HandleResume(GenericOperationalError &err)
{
    if (!m_connected || !m_can_resume || post_command(id(), CommandKind::Resume) != ESP_OK) {
        err.Set(to_underlying(ErrorStateEnum::kUnableToStartOrResume));
        return;
    }
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

void DishwasherDevice::HandleChangeToMode(uint8_t new_mode, ModeBase::Commands::ChangeToModeResponse::Type &response)
{
    if (new_mode >= m_stored.programs.size()) {
        response.status = to_underlying(ModeBase::StatusCode::kUnsupportedMode);
        return;
    }
    if (m_operation_state == HcOperationState::Run || m_operation_state == HcOperationState::Pause) {
        response.status = to_underlying(ModeBase::StatusCode::kInvalidInMode);
        response.statusText.SetValue("Cannot change program while running"_span);
        return;
    }
    if (!m_connected || m_remote_control_active == 0) {
        response.status = to_underlying(ModeBase::StatusCode::kGenericFailure);
        response.statusText.SetValue("Remote control is not active on the appliance"_span);
        return;
    }
    if (post_command(id(), CommandKind::SelectProgram, m_stored.programs[new_mode].key.c_str()) != ESP_OK) {
        response.status = to_underlying(ModeBase::StatusCode::kGenericFailure);
        return;
    }
    // The mode server sets CurrentMode on success. If HomeConnect then rejects
    // the selection, the job's Sync() moves it back.
    response.status = to_underlying(ModeBase::StatusCode::kSuccess);
}

esp_err_t DishwasherDevice::OnAttributeWrite(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        const bool on = val->val.b;
        if ((int)on != m_reported_on_off) {
            m_reported_on_off = on;
            return post_command(id(), CommandKind::Power, "", on);
        }
    }
    return ESP_OK;
}

void DishwasherDevice::RequestRefresh()
{
    char *arg = strdup(id());
    if (arg != nullptr && cloud_worker_post(run_refresh_job, arg) != ESP_OK) {
        free(arg);
    }
}

void DishwasherDevice::ToJson(cJSON *obj)
{
    BridgedAppliance::ToJson(obj);
    cJSON_AddStringToObject(obj, "kind", "dishwasher");
    cJSON_AddStringToObject(obj, "operationState", operation_state_name(m_operation_state));
    cJSON_AddStringToObject(obj, "doorState", m_door_state.c_str());
    cJSON_AddStringToObject(obj, "powerState", m_power_state.c_str());
    if (m_remote_start_allowed >= 0) {
        cJSON_AddBoolToObject(obj, "remoteStartAllowed", m_remote_start_allowed == 1);
    }
    if (m_remote_control_active >= 0) {
        cJSON_AddBoolToObject(obj, "remoteControlActive", m_remote_control_active == 1);
    }
    if (m_remaining_s >= 0) {
        cJSON_AddNumberToObject(obj, "remainingSeconds", m_remaining_s);
    }
    if (m_progress_pct >= 0) {
        cJSON_AddNumberToObject(obj, "progress", m_progress_pct);
    }
    cJSON_AddStringToObject(obj, "activeProgram", m_active_program.c_str());
    cJSON_AddStringToObject(obj, "selectedProgram", m_selected_program.c_str());
    cJSON_AddBoolToObject(obj, "saltLow", m_salt_low);
    cJSON_AddBoolToObject(obj, "rinseAidLow", m_rinse_aid_low);
    cJSON_AddBoolToObject(obj, "canPause", m_can_pause);
    cJSON_AddStringToObject(obj, "lastError", m_last_command_error.c_str());

    ModeBase::Instance *mode = m_mode_delegate.instance();
    if (mode != nullptr) {
        cJSON_AddNumberToObject(obj, "currentMode", mode->GetCurrentMode());
    }

    cJSON *programs = cJSON_AddArrayToObject(obj, "programs");
    for (size_t i = 0; i < m_stored.programs.size(); i++) {
        char label[65];
        label_for_program(m_stored.programs[i], label, sizeof(label));
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "mode", (double)i);
        cJSON_AddStringToObject(p, "key", m_stored.programs[i].key.c_str());
        cJSON_AddStringToObject(p, "label", label);
        cJSON_AddItemToArray(programs, p);
    }
}
