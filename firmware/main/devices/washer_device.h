#pragma once

#include <string>

#include <esp_matter.h>

#include <app-common/zap-generated/cluster-enums.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/operational-state-server/operational-state-server.h>

#include "bridged_appliance.h"

/**
 * Maps a SmartThings washer (Samsung) onto a Matter Laundry Washer endpoint:
 *
 *   OperationalState   <- washerOperatingState.machineState / washerJobState /
 *                         completionTime; Start/Stop/Pause/Resume ->
 *                         washerOperatingState.setMachineState
 *   LaundryWasherMode  <- samsungce.washerCycle (keyword-matched to 4 modes)
 *   OnOff (DeadFront)  <-> switch
 *
 * Commands need Remote Control enabled on the washer
 * (remoteControlStatus.remoteControlEnabled).
 */

class WasherDevice;

class WasherOperationalStateDelegate : public chip::app::Clusters::OperationalState::Delegate
{
public:
    explicit WasherOperationalStateDelegate(WasherDevice &device) : m_device(device) {}

    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override;
    CHIP_ERROR GetOperationalStateAtIndex(
        size_t index, chip::app::Clusters::OperationalState::GenericOperationalState &operationalState) override;
    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override;

    void HandlePauseStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override;
    void HandleResumeStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override;
    void HandleStartStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override;
    void HandleStopStateCallback(chip::app::Clusters::OperationalState::GenericOperationalError &err) override;

    chip::app::Clusters::OperationalState::Instance *instance() { return GetInstance(); }

private:
    WasherDevice &m_device;
};

class WasherModeDelegate : public chip::app::Clusters::ModeBase::Delegate
{
public:
    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }
    void HandleChangeToMode(uint8_t new_mode,
                            chip::app::Clusters::ModeBase::Commands::ChangeToModeResponse::Type &response) override;
    CHIP_ERROR GetModeLabelByIndex(uint8_t mode_index, chip::MutableCharSpan &label) override;
    CHIP_ERROR GetModeValueByIndex(uint8_t mode_index, uint8_t &value) override;
    CHIP_ERROR GetModeTagsByIndex(
        uint8_t mode_index,
        chip::app::DataModel::List<chip::app::Clusters::detail::Structs::ModeTagStruct::Type> &tags) override;

    chip::app::Clusters::ModeBase::Instance *instance() { return GetInstance(); }
};

class WasherDevice : public BridgedAppliance
{
public:
    explicit WasherDevice(const StoredAppliance &stored);

    uint32_t device_type_id() const override;
    esp_err_t AddClusters(esp_matter::endpoint_t *ep) override;
    void OnEndpointReady() override;
    void ApplyAttribute(const char *capability, const char *attribute, const cJSON *value) override;
    void Sync() override;
    void RequestRefresh() override;
    void ToJson(cJSON *obj) override;
    esp_err_t OnAttributeWrite(uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val) override;

    // ---- Delegate support (Matter task, lock held) ----
    chip::app::DataModel::Nullable<uint32_t> CountdownTime() const;
    void HandleMachineState(const char *target, chip::app::Clusters::OperationalState::GenericOperationalError &err);

    // ---- Worker results (lock held) ----
    void SetLastCommandError(const char *message) { m_last_command_error = message != nullptr ? message : ""; }

private:
    void SyncOperationalState();
    void SyncMode();
    void SyncOnOff();

    WasherOperationalStateDelegate m_operational_state_delegate;
    WasherModeDelegate m_mode_delegate;

    // ---- Cached SmartThings state ----
    std::string m_machine_state; // "run", "pause", "stop" or ""
    std::string m_job_state;     // washerJobState, e.g. "wash"
    std::string m_cycle;         // samsungce.washerCycle / washerMode
    std::string m_switch;        // "on", "off" or ""
    std::string m_door;          // "open", "closed" or ""
    int m_remote_control = -1;   // -1 unknown, else bool
    time_t m_completion_time = 0; // UTC epoch, 0 unknown
    std::string m_last_command_error;

    // ---- What has been pushed to Matter ----
    bool m_endpoint_ready = false;
    bool m_was_running = false;
    int m_reported_on_off = -1;
};
