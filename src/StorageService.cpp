#include "StorageService.h"

bool StorageService::Contains(
    const String& key)
{
    _preferences.begin(
        Namespace,
        true);

    bool found =
        _preferences.isKey(
            key.c_str());

    _preferences.end();

    return found;
}

String StorageService::GetString(
    const String& key,
    const String& defaultValue)
{
    _preferences.begin(
        Namespace,
        true);

    auto value =
        _preferences.getString(
            key.c_str(),
            defaultValue);

    _preferences.end();

    return value;
}

void StorageService::SaveString(
    const String& key,
    const String& value)
{
    _preferences.begin(
        Namespace,
        false);

    _preferences.putString(
        key.c_str(),
        value);

    _preferences.end();
}

int StorageService::GetInt(
    const String& key,
    int defaultValue)
{
    _preferences.begin(
        Namespace,
        true);

    auto value =
        _preferences.getInt(
            key.c_str(),
            defaultValue);

    _preferences.end();

    return value;
}

void StorageService::SaveInt(
    const String& key,
    int value)
{
    _preferences.begin(
        Namespace,
        false);

    _preferences.putInt(
        key.c_str(),
        value);

    _preferences.end();
}

void StorageService::Remove(
    const String& key)
{
    _preferences.begin(
        Namespace,
        false);

    _preferences.remove(
        key.c_str());

    _preferences.end();
}

void StorageService::Clear()
{
    _preferences.begin(
        Namespace,
        false);

    _preferences.clear();

    _preferences.end();
}

bool StorageService::HasWifiCredentials()
{
    return Contains(
        WifiSsid);
}

String StorageService::GetWifiSsid()
{
    return GetString(
        WifiSsid);
}

String StorageService::GetWifiPassword()
{
    return GetString(
        WifiPassword);
}

void StorageService::SaveWifiCredentials(
    const String& ssid,
    const String& password)
{
    SaveString(
        WifiSsid,
        ssid);

    SaveString(
        WifiPassword,
        password);
}


bool StorageService::LoadProvisioningConfig(
    ProvisioningConfig& config)
{
    _preferences.begin(
        Namespace,
        true);

    bool provisioned =
        _preferences.getInt(ProvisioningState, 0) == 1;

    if (provisioned)
    {
        config.wifiSsid =
            _preferences.getString(WifiSsid, "");

        config.wifiPassword =
            _preferences.getString(WifiPassword, "");

        config.controlServerUrl =
            _preferences.getString(ControlServerUrl, "");
    }

    _preferences.end();

    return provisioned &&
        config.wifiSsid.length() > 0 &&
        config.controlServerUrl.length() > 0;
}


bool StorageService::SaveProvisioningConfig(
    const ProvisioningConfig& config)
{
    if (!_preferences.begin(
            Namespace,
            false))
    {
        return false;
    }

    // Removed first: from here until the last write the board is not
    // provisioned, whatever else is on flash.
    _preferences.remove(ProvisioningState);

    _preferences.putString(WifiSsid, config.wifiSsid);
    _preferences.putString(WifiPassword, config.wifiPassword);
    _preferences.putString(ControlServerUrl, config.controlServerUrl);
    _preferences.putInt(ProvisioningVersion, CurrentProvisioningVersion);

    // Verified by reading back rather than from the put* return values:
    // putString returns the length written, which is 0 for an open network's
    // empty password whether or not it succeeded.
    bool written =
        _preferences.getString(WifiSsid, "") == config.wifiSsid &&
        _preferences.isKey(WifiPassword) &&
        _preferences.getString(WifiPassword, "") == config.wifiPassword &&
        _preferences.getString(ControlServerUrl, "") == config.controlServerUrl &&
        _preferences.getInt(ProvisioningVersion, 0) == CurrentProvisioningVersion;

    // The commit marker, last.
    if (written)
    {
        written =
            _preferences.putInt(ProvisioningState, 1) > 0 &&
            _preferences.getInt(ProvisioningState, 0) == 1;
    }

    _preferences.end();

    return written;
}


void StorageService::ClearProvisioningConfig()
{
    _preferences.begin(
        Namespace,
        false);

    _preferences.remove(ProvisioningState);
    _preferences.remove(WifiSsid);
    _preferences.remove(WifiPassword);
    _preferences.remove(ControlServerUrl);
    _preferences.remove(ProvisioningVersion);

    _preferences.end();
}
