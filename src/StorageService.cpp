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