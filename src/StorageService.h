#pragma once

#include <Arduino.h>
#include <Preferences.h>

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

private:

    static constexpr const char* Namespace = "homeshield";

    static constexpr const char* WifiSsid = "wifi_ssid";

    static constexpr const char* WifiPassword = "wifi_pwd";

    Preferences _preferences;
};