#include "washer_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <esp_matter_attribute_utils.h>
#include <esp_matter_endpoint.h>

#include "esp_log.h"

#include "bridge/bridge_manager.h"
#include "cloud_worker.h"
#include "smartthings/st_client.h"
#include "web_server.h"

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::OperationalState;
using namespace esp_matter;
using chip::Protocols::InteractionModel::Status;
using ModeTagStructType = chip::app::Clusters::detail::Structs::ModeTagStruct::Type;

#define TAG "washer"

// Anything before this means SNTP has not set the clock yet.
#define MIN_VALID_EPOCH 1704067200 // 2024-01-01
#define MAX_COUNTDOWN_S (24 * 3600)

namespace {

// ---------------------------------------------------------------------------
// Phases and modes (from matter-esp32-smartthings-adapter)
// ---------------------------------------------------------------------------

// washerJobState values, passed through as the PhaseList: Matter defines no
// phase vocabulary, so SmartThings' own names are the most useful labels.
const CharSpan kPhases[] = {
    "preWash"_span, "wash"_span,    "rinse"_span,         "spin"_span,
    "drying"_span,  "cooling"_span, "weightSensing"_span, "wrinklePrevent"_span,
    "finish"_span,
};
constexpr size_t kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);

int phase_index_for(const std::string &job_state)
{
    for (size_t i = 0; i < kPhaseCount; i++) {
        if (kPhases[i].data_equal(CharSpan(job_state.data(), job_state.size()))) {
            return (int)i;
        }
    }
    return -1;
}

const GenericOperationalState kOperationalStates[] = {
    GenericOperationalState(to_underlying(OperationalStateEnum::kStopped)),
    GenericOperationalState(to_underlying(OperationalStateEnum::kRunning)),
    GenericOperationalState(to_underlying(OperationalStateEnum::kPaused)),
    GenericOperationalState(to_underlying(OperationalStateEnum::kError)),
};

/**
 * Samsung reports the selected cycle as an opaque course code
 * ("Table_00_Course_D2") on many models, so the cycle cannot be enumerated
 * as modes. Four standard modes are exposed and the cycle is matched to the
 * closest by keyword.
 */
struct WasherMode {
    const char *label;
    LaundryWasherMode::ModeTag tag;
};

const WasherMode kModes[] = {
    {"Normal", LaundryWasherMode::ModeTag::kNormal},
    {"Delicate", LaundryWasherMode::ModeTag::kDelicate},
    {"Heavy", LaundryWasherMode::ModeTag::kHeavy},
    {"Whites", LaundryWasherMode::ModeTag::kWhites},
};
constexpr uint8_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

bool contains_ignore_case(const std::string &haystack, const char *needle)
{
    const size_t needle_len = strlen(needle);
    for (size_t i = 0; i + needle_len <= haystack.size(); i++) {
        if (strncasecmp(haystack.c_str() + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

uint8_t mode_for_cycle(const std::string &cycle)
{
    for (const char *k : {"delicate", "wool", "silk", "hand"}) {
        if (contains_ignore_case(cycle, k)) return 1;
    }
    for (const char *k : {"heavy", "bedding", "towel", "intensive"}) {
        if (contains_ignore_case(cycle, k)) return 2;
    }
    for (const char *k : {"white", "cotton"}) {
        if (contains_ignore_case(cycle, k)) return 3;
    }
    return 0;
}

/** SmartThings sends most values as strings; remoteControlEnabled as "true"/"false". */
std::string string_value(const cJSON *value)
{
    if (cJSON_IsString(value)) {
        return value->valuestring;
    }
    if (cJSON_IsBool(value)) {
        return cJSON_IsTrue(value) ? "true" : "false";
    }
    return "";
}

void notify_ui(const char *id)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"type\":\"appliance\",\"id\":\"%s\"}", id);
    web_server_notify_ws_event(msg);
}

// ---------------------------------------------------------------------------
// Worker jobs: HTTPS off the CHIP lock, results applied under it.
// ---------------------------------------------------------------------------

struct CommandJob {
    char device_id[APPLIANCE_ID_LEN];
    char capability[40];
    char command[24];
    char arguments[48];
};

void run_command_job(void *arg)
{
    CommandJob *job = static_cast<CommandJob *>(arg);
    esp_err_t err = st_client_command(job->device_id, job->capability, job->command,
                                      job->arguments[0] ? job->arguments : nullptr);

    char error[224] = "";
    if (err != ESP_OK) {
        char detail[112];
        st_client_last_error(detail, sizeof(detail));
        snprintf(error, sizeof(error), "%s.%s failed: %s", job->capability, job->command, detail);
        ESP_LOGW(TAG, "%s: %s", job->device_id, error);
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    BridgedAppliance *appliance = BridgeManager::instance().find(APPLIANCE_SOURCE_SMARTTHINGS, job->device_id);
    if (appliance != nullptr && appliance->device_type_id() == ESP_MATTER_LAUNDRY_WASHER_DEVICE_TYPE_ID) {
        auto *washer = static_cast<WasherDevice *>(appliance);
        washer->SetLastCommandError(error);
        // On failure, put OnOff back to what the washer reports.
        washer->Sync();
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    notify_ui(job->device_id);
    free(job);
}

esp_err_t post_command(const char *device_id, const char *capability, const char *command, const char *arguments)
{
    CommandJob *job = static_cast<CommandJob *>(calloc(1, sizeof(CommandJob)));
    if (job == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(job->device_id, device_id, sizeof(job->device_id));
    strlcpy(job->capability, capability, sizeof(job->capability));
    strlcpy(job->command, command, sizeof(job->command));
    strlcpy(job->arguments, arguments != nullptr ? arguments : "", sizeof(job->arguments));
    esp_err_t err = cloud_worker_post(run_command_job, job);
    if (err != ESP_OK) {
        free(job);
    }
    return err;
}

void run_refresh_job(void *arg)
{
    char *device_id = static_cast<char *>(arg);

    bool online = false;
    const esp_err_t health_err = st_client_get_health(device_id, &online);
    cJSON *root = nullptr;
    const cJSON *main = nullptr;
    const esp_err_t status_err = st_client_get_status(device_id, &root, &main);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    BridgedAppliance *appliance = BridgeManager::instance().find(APPLIANCE_SOURCE_SMARTTHINGS, device_id);
    if (appliance != nullptr) {
        if (health_err == ESP_OK) {
            appliance->SetConnected(online);
        } else if (status_err == ESP_OK) {
            appliance->SetConnected(true);
        }
        if (status_err == ESP_OK) {
            const cJSON *capability = nullptr;
            cJSON_ArrayForEach(capability, main)
            {
                const cJSON *attribute = nullptr;
                cJSON_ArrayForEach(attribute, capability)
                {
                    appliance->ApplyAttribute(capability->string, attribute->string,
                                              cJSON_GetObjectItemCaseSensitive(attribute, "value"));
                }
            }
        }
        appliance->Sync();
    }
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Refreshed %s (%s)", device_id,
             status_err == ESP_OK ? (online ? "online" : "status read") : esp_err_to_name(status_err));
    notify_ui(device_id);
    free(device_id);
}

} // namespace

// ---------------------------------------------------------------------------
// Operational State delegate
// ---------------------------------------------------------------------------

DataModel::Nullable<uint32_t> WasherOperationalStateDelegate::GetCountdownTime()
{
    return m_device.CountdownTime();
}

CHIP_ERROR WasherOperationalStateDelegate::GetOperationalStateAtIndex(size_t index,
                                                                     GenericOperationalState &operationalState)
{
    if (index >= sizeof(kOperationalStates) / sizeof(kOperationalStates[0])) {
        return CHIP_ERROR_NOT_FOUND;
    }
    operationalState = kOperationalStates[index];
    return CHIP_NO_ERROR;
}

CHIP_ERROR WasherOperationalStateDelegate::GetOperationalPhaseAtIndex(size_t index, MutableCharSpan &operationalPhase)
{
    if (index >= kPhaseCount) {
        return CHIP_ERROR_NOT_FOUND;
    }
    return CopyCharSpanToMutableCharSpan(kPhases[index], operationalPhase);
}

void WasherOperationalStateDelegate::HandlePauseStateCallback(GenericOperationalError &err)
{
    m_device.HandleMachineState("pause", err);
}

void WasherOperationalStateDelegate::HandleResumeStateCallback(GenericOperationalError &err)
{
    m_device.HandleMachineState("run", err);
}

void WasherOperationalStateDelegate::HandleStartStateCallback(GenericOperationalError &err)
{
    m_device.HandleMachineState("run", err);
}

void WasherOperationalStateDelegate::HandleStopStateCallback(GenericOperationalError &err)
{
    m_device.HandleMachineState("stop", err);
}

// ---------------------------------------------------------------------------
// Laundry Washer Mode delegate
// ---------------------------------------------------------------------------

void WasherModeDelegate::HandleChangeToMode(uint8_t new_mode, ModeBase::Commands::ChangeToModeResponse::Type &response)
{
    // The modes are a reading of the selected cycle, not something SmartThings
    // can set: Samsung cycles are model-specific codes.
    response.status = to_underlying(ModeBase::StatusCode::kGenericFailure);
    response.statusText.SetValue("Select the cycle on the washer"_span);
}

CHIP_ERROR WasherModeDelegate::GetModeLabelByIndex(uint8_t mode_index, MutableCharSpan &label)
{
    if (mode_index >= kModeCount) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    return CopyCharSpanToMutableCharSpan(CharSpan::fromCharString(kModes[mode_index].label), label);
}

CHIP_ERROR WasherModeDelegate::GetModeValueByIndex(uint8_t mode_index, uint8_t &value)
{
    if (mode_index >= kModeCount) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    value = mode_index;
    return CHIP_NO_ERROR;
}

CHIP_ERROR WasherModeDelegate::GetModeTagsByIndex(uint8_t mode_index, DataModel::List<ModeTagStructType> &tags)
{
    if (mode_index >= kModeCount) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    if (tags.size() < 1) {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    tags[0] = ModeTagStructType{.value = to_underlying(kModes[mode_index].tag)};
    tags.reduce_size(1);
    return CHIP_NO_ERROR;
}

// ---------------------------------------------------------------------------
// Washer device
// ---------------------------------------------------------------------------

WasherDevice::WasherDevice(const StoredAppliance &stored) : BridgedAppliance(stored), m_operational_state_delegate(*this)
{}

uint32_t WasherDevice::device_type_id() const
{
    return ESP_MATTER_LAUNDRY_WASHER_DEVICE_TYPE_ID;
}

esp_err_t WasherDevice::AddClusters(endpoint_t *ep)
{
    BridgedAppliance::AddClusters(ep);

    endpoint::laundry_washer::config_t washer_config;
    washer_config.operational_state.delegate = &m_operational_state_delegate;
    esp_err_t err = endpoint::laundry_washer::add(ep, &washer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add laundry washer clusters: %s", esp_err_to_name(err));
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

    cluster::laundry_washer_mode::config_t mode_config;
    mode_config.delegate = &m_mode_delegate;
    mode_config.current_mode = 0;
    cluster_t *mode = cluster::laundry_washer_mode::create(ep, &mode_config, CLUSTER_FLAG_SERVER);
    if (mode != nullptr) {
        cluster::mode_base::attribute::create_supported_modes(mode, NULL, 0, 0);
        cluster::mode_base::command::create_change_to_mode(mode);
    }

    cluster::on_off::config_t on_off_config;
    cluster_t *on_off = cluster::on_off::create(ep, &on_off_config, CLUSTER_FLAG_SERVER);
    if (on_off != nullptr) {
        cluster::on_off::feature::dead_front_behavior::add(on_off);
        cluster::on_off::command::create_on(on_off);
        cluster::on_off::command::create_toggle(on_off);
    }
    return ESP_OK;
}

void WasherDevice::OnEndpointReady()
{
    m_endpoint_ready = true;
    Sync();
}

void WasherDevice::ApplyAttribute(const char *capability, const char *attribute, const cJSON *value)
{
    if (capability == nullptr || attribute == nullptr) {
        return;
    }
    const std::string cap = capability;
    const std::string attr = attribute;

    if (cap == "washerOperatingState") {
        if (attr == "machineState") {
            m_machine_state = string_value(value);
        } else if (attr == "washerJobState") {
            m_job_state = string_value(value);
        } else if (attr == "completionTime") {
            time_t t = 0;
            m_completion_time = st_parse_iso8601_utc(cJSON_GetStringValue(value), &t) ? t : 0;
        }
    } else if (cap == "samsungce.washerCycle" && attr == "washerCycle") {
        m_cycle = string_value(value);
    } else if (cap == "washerMode" && attr == "washerMode" && m_cycle.empty()) {
        m_cycle = string_value(value);
    } else if (cap == "switch" && attr == "switch") {
        m_switch = string_value(value);
    } else if (cap == "remoteControlStatus" && attr == "remoteControlEnabled") {
        const std::string v = string_value(value);
        m_remote_control = v == "true" ? 1 : v == "false" ? 0 : -1;
    } else if (cap == "samsungce.doorState" && attr == "doorState") {
        m_door = string_value(value);
    } else if (cap == "contactSensor" && attr == "contact" && m_door.empty()) {
        m_door = string_value(value);
    }
}

DataModel::Nullable<uint32_t> WasherDevice::CountdownTime() const
{
    const bool in_cycle = m_machine_state == "run" || m_machine_state == "pause";
    const time_t now = time(nullptr);
    if (!in_cycle || m_completion_time == 0 || now < MIN_VALID_EPOCH || m_completion_time <= now ||
        m_completion_time - now > MAX_COUNTDOWN_S) {
        return DataModel::NullNullable;
    }
    return DataModel::MakeNullable((uint32_t)(m_completion_time - now));
}

void WasherDevice::Sync()
{
    if (!m_endpoint_ready) {
        return;
    }
    SyncOperationalState();
    SyncMode();
    SyncOnOff();
}

void WasherDevice::SyncOperationalState()
{
    OperationalState::Instance *instance = m_operational_state_delegate.instance();
    if (instance == nullptr || m_machine_state.empty()) {
        return;
    }

    OperationalStateEnum state;
    if (m_machine_state == "run") {
        state = OperationalStateEnum::kRunning;
    } else if (m_machine_state == "pause") {
        state = OperationalStateEnum::kPaused;
    } else {
        state = OperationalStateEnum::kStopped;
    }

    const bool running = state != OperationalStateEnum::kStopped;
    if (m_was_running && !running) {
        instance->OnOperationCompletionDetected(to_underlying(ErrorStateEnum::kNoError));
    }
    m_was_running = running;

    CHIP_ERROR err = instance->SetOperationalState(to_underlying(state));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "%s: SetOperationalState failed: %" CHIP_ERROR_FORMAT, id(), err.Format());
    }

    const int phase = running ? phase_index_for(m_job_state) : -1;
    err = phase < 0 ? instance->SetCurrentPhase(DataModel::NullNullable)
                    : instance->SetCurrentPhase(DataModel::MakeNullable((uint8_t)phase));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "%s: SetCurrentPhase failed: %" CHIP_ERROR_FORMAT, id(), err.Format());
    }
    instance->UpdateCountdownTimeFromDelegate();
}

void WasherDevice::SyncMode()
{
    ModeBase::Instance *instance = m_mode_delegate.instance();
    if (instance == nullptr || m_cycle.empty()) {
        return;
    }
    const uint8_t mode = mode_for_cycle(m_cycle);
    if (instance->GetCurrentMode() != mode) {
        instance->UpdateCurrentMode(mode);
    }
}

void WasherDevice::SyncOnOff()
{
    if (m_switch.empty()) {
        return;
    }
    const int on = m_switch == "on" ? 1 : 0;
    if (on == m_reported_on_off) {
        return;
    }
    m_reported_on_off = on;
    // report(), not update(): the change came from SmartThings and must not
    // loop back through app_attribute_update_cb.
    esp_matter_attr_val_t val = esp_matter_bool(on == 1);
    attribute::report(endpoint_id(), OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

void WasherDevice::HandleMachineState(const char *target, GenericOperationalError &err)
{
    const bool resuming = strcmp(target, "run") == 0;
    const auto refused = resuming ? ErrorStateEnum::kUnableToStartOrResume : ErrorStateEnum::kCommandInvalidInState;

    // Samsung washers only accept remote commands with Remote Control on.
    if (!m_connected || m_remote_control == 0) {
        err.Set(to_underlying(refused));
        return;
    }
    char arguments[24];
    snprintf(arguments, sizeof(arguments), "[\"%s\"]", target);
    if (post_command(id(), "washerOperatingState", "setMachineState", arguments) != ESP_OK) {
        err.Set(to_underlying(refused));
        return;
    }
    err.Set(to_underlying(ErrorStateEnum::kNoError));
}

esp_err_t WasherDevice::OnAttributeWrite(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        const bool on = val->val.b;
        if ((int)on != m_reported_on_off) {
            m_reported_on_off = on;
            return post_command(id(), "switch", on ? "on" : "off", nullptr);
        }
    }
    return ESP_OK;
}

void WasherDevice::RequestRefresh()
{
    char *arg = strdup(id());
    if (arg != nullptr && cloud_worker_post(run_refresh_job, arg) != ESP_OK) {
        free(arg);
    }
}

void WasherDevice::ToJson(cJSON *obj)
{
    BridgedAppliance::ToJson(obj);
    cJSON_AddStringToObject(obj, "kind", "washer");
    cJSON_AddStringToObject(obj, "machineState", m_machine_state.c_str());
    cJSON_AddStringToObject(obj, "jobState", m_job_state.c_str());
    cJSON_AddStringToObject(obj, "cycle", m_cycle.c_str());
    cJSON_AddStringToObject(obj, "switch", m_switch.c_str());
    cJSON_AddStringToObject(obj, "doorState", m_door.c_str());
    if (m_remote_control >= 0) {
        cJSON_AddBoolToObject(obj, "remoteControlEnabled", m_remote_control == 1);
    }
    DataModel::Nullable<uint32_t> countdown = CountdownTime();
    if (!countdown.IsNull()) {
        cJSON_AddNumberToObject(obj, "remainingSeconds", countdown.Value());
    }
    cJSON_AddStringToObject(obj, "mode", m_cycle.empty() ? "" : kModes[mode_for_cycle(m_cycle)].label);
    cJSON_AddStringToObject(obj, "lastError", m_last_command_error.c_str());
}
