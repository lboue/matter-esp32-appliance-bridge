#pragma once

#include <string>

#include <esp_matter.h>

#include <app-common/zap-generated/cluster-enums.h>
#include <app/clusters/dishwasher-alarm-server/dishwasher-alarm-delegate.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/operational-state-server/operational-state-server.h>

#include "bridged_appliance.h"

/**
 * Maps a HomeConnect dishwasher onto a Matter Dishwasher endpoint:
 *
 *   OperationalState  <- BSH.Common.Status.OperationState, RemainingProgramTime;
 *                        Start/Stop/Pause/Resume -> programs/active, commands
 *   DishwasherMode    <- one mode per HomeConnect program;
 *                        ChangeToMode -> programs/selected
 *   OnOff (DeadFront) <-> BSH.Common.Setting.PowerState
 *   DishwasherAlarm   <- DoorError while the door is open mid-program
 *
 * One instance per bridged dishwasher; the delegates are members so a second
 * dishwasher never shares the first one's state.
 */

class DishwasherDevice;

class DishwasherOperationalStateDelegate : public chip::app::Clusters::OperationalState::Delegate
{
public:
    explicit DishwasherOperationalStateDelegate(DishwasherDevice &device) : m_device(device) {}

    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override;
    CHIP_ERROR GetOperationalStateAtIndex(
        size_t index, chip::app::Clusters::OperationalState::GenericOperationalState &operationalState) override;
    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override;

    void HandlePauseStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override;
    void HandleResumeStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override;
    void HandleStartStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override;
    void HandleStopStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override;

    // GetInstance() is protected on Delegate. Null until the cluster's
    // delegate init callback has run.
    chip::app::Clusters::OperationalState::Instance *instance() { return GetInstance(); }

private:
    DishwasherDevice &m_device;
};

class DishwasherModeDelegate : public chip::app::Clusters::ModeBase::Delegate
{
public:
    explicit DishwasherModeDelegate(DishwasherDevice &device) : m_device(device) {}

    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }
    void HandleChangeToMode(uint8_t new_mode,
                            chip::app::Clusters::ModeBase::Commands::ChangeToModeResponse::Type &response) override;
    CHIP_ERROR GetModeLabelByIndex(uint8_t mode_index, chip::MutableCharSpan &label) override;
    CHIP_ERROR GetModeValueByIndex(uint8_t mode_index, uint8_t &value) override;
    CHIP_ERROR GetModeTagsByIndex(
        uint8_t mode_index,
        chip::app::DataModel::List<chip::app::Clusters::detail::Structs::ModeTagStruct::Type> &tags) override;

    chip::app::Clusters::ModeBase::Instance *instance() { return GetInstance(); }

private:
    DishwasherDevice &m_device;
};

class DishwasherAlarmDelegate : public chip::app::Clusters::DishwasherAlarm::Delegate
{
public:
    // The alarms mirror the appliance, so enabling/resetting them is accepted
    // but has nothing to act on.
    bool ModifyEnabledAlarmsCallback(const chip::BitMask<chip::app::Clusters::DishwasherAlarm::AlarmMap> mask) override
    {
        return true;
    }
    bool ResetAlarmsCallback(const chip::BitMask<chip::app::Clusters::DishwasherAlarm::AlarmMap> alarms) override
    {
        return true;
    }
};

class DishwasherDevice : public BridgedAppliance
{
public:
    // BSH.Common.EnumType.OperationState.*
    enum class HcOperationState : uint8_t {
        Unknown,
        Inactive,
        Ready,
        DelayedStart,
        Run,
        Pause,
        ActionRequired,
        Finished,
        Error,
        Aborting,
    };

    explicit DishwasherDevice(const StoredAppliance &stored);

    uint32_t device_type_id() const override;
    esp_err_t AddClusters(esp_matter::endpoint_t *ep) override;
    void OnEndpointReady() override;
    void ApplyItem(const char *key, const cJSON *value) override;
    void Sync() override;
    void RequestRefresh() override;
    void ToJson(cJSON *obj) override;
    esp_err_t OnAttributeWrite(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val) override;

    // ---- Delegate support (Matter task, lock held) ----
    chip::app::DataModel::Nullable<uint32_t> CountdownTime() const;
    void HandleStart(chip::app::Clusters::OperationalState::GenericOperationalError &err);
    void HandleStop(chip::app::Clusters::OperationalState::GenericOperationalError &err);
    void HandlePause(chip::app::Clusters::OperationalState::GenericOperationalError &err);
    void HandleResume(chip::app::Clusters::OperationalState::GenericOperationalError &err);
    void HandleChangeToMode(uint8_t new_mode,
                            chip::app::Clusters::ModeBase::Commands::ChangeToModeResponse::Type &response);
    size_t ModeCount() const { return m_stored.programs.size(); }
    const HcProgram &ProgramAt(size_t index) const { return m_stored.programs[index]; }

    // ---- Snapshot / command results (worker task, lock held) ----
    /** Append any programs not yet known. Returns true if the list grew. */
    bool MergePrograms(const cJSON *programs);
    void SetActiveProgram(const char *key);
    void SetSelectedProgram(const char *key);
    void SetCommandSupport(bool pause, bool resume);
    void SetLastCommandError(const char *message);

private:
    int ModeForProgram(const std::string &key) const;
    void SyncOperationalState();
    void SyncMode();
    void SyncOnOff();
    void SyncAlarm();

    DishwasherOperationalStateDelegate m_operational_state_delegate;
    DishwasherModeDelegate m_mode_delegate;
    DishwasherAlarmDelegate m_alarm_delegate;

    // ---- Cached HomeConnect state ----
    HcOperationState m_operation_state = HcOperationState::Unknown;
    std::string m_door_state;  // "Open", "Closed", "Locked" or ""
    std::string m_power_state; // "On", "Off", "Standby" or ""
    int m_remote_start_allowed = -1; // -1 unknown, else bool
    int m_remote_control_active = -1;
    int m_remaining_s = -1;
    int m_progress_pct = -1;
    std::string m_active_program;
    std::string m_selected_program;
    bool m_salt_low = false;
    bool m_rinse_aid_low = false;
    bool m_can_pause = false;
    bool m_can_resume = false;
    std::string m_last_command_error;

    // ---- What has been pushed to Matter ----
    bool m_endpoint_ready = false;
    bool m_error_reported = false;
    bool m_was_running = false;
    int m_reported_on_off = -1;
};
