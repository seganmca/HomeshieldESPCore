#pragma once

#include <Arduino.h>
#include <Preferences.h>

// Milestone 37. What BLE onboarding delivers and what a provisioned board
// boots from.
struct ProvisioningConfig
{
    String wifiSsid;
    String wifiPassword;
    String controlServerUrl;
};


class StorageService
{
public:

    bool Contains(
        const String& key);

    String GetString(
        const String& key,
        const String& defaultValue = "");

    void SaveString(
        const String& key,
        const String& value);

    int GetInt(
        const String& key,
        int defaultValue = 0);

    void SaveInt(
        const String& key,
        int value);

    void Remove(
        const String& key);

    void Clear();

    bool HasWifiCredentials();

    String GetWifiSsid();

    String GetWifiPassword();

    void SaveWifiCredentials(
        const String& ssid,
        const String& password);

    // --------------------------------------------------
    // Provisioning configuration (milestone 37)
    // --------------------------------------------------
    //
    // A board is provisioned only when prov_state == 1. That key is written
    // LAST by SaveProvisioningConfig and removed FIRST, so a power cut in the
    // middle of a save leaves the board unprovisioned rather than half
    // configured. A board carrying only the pre-M37 Wi-Fi keys is therefore
    // unprovisioned and is onboarded over BLE.

    // True and fills config only when the board is provisioned.
    bool LoadProvisioningConfig(
        ProvisioningConfig& config);

    // False if any write could not be verified. The commit marker is then
    // absent, so the board stays unprovisioned.
    bool SaveProvisioningConfig(
        const ProvisioningConfig& config);

    // Removes the provisioning keys, and the Sensor Hub node registry with
    // them. The hardware identity is the eFuse MAC and is never stored here, so
    // nothing about it is touched.
    //
    // Milestone 38 (decision A38-8). The node registry lives in its own
    // namespace and would otherwise survive a reset - and a hub being
    // re-onboarded is starting over, possibly in a different home. Carrying the
    // old children across would declare a stranger's sensors to a new Control
    // Server and create child Devices for hardware that is not there.
    void ClearProvisioningConfig();

private:

    static constexpr const char* ControlServerUrl = "cs_url";

    static constexpr const char* ProvisioningVersion = "prov_ver";

    static constexpr const char* ProvisioningState = "prov_state";

    static constexpr int CurrentProvisioningVersion = 1;


    static constexpr const char* Namespace = "homeshield";

    // Milestone 38. The Sensor Hub's node registry, cleared alongside the
    // provisioning configuration. Declared here rather than only in EspNowHub
    // so the two agree about the name; EspNowHub owns the schema inside it.
    static constexpr const char* NodeRegistryNamespace = "hs_nodes";

    static constexpr const char* WifiSsid = "wifi_ssid";

    static constexpr const char* WifiPassword = "wifi_pwd";

    Preferences _preferences;
};