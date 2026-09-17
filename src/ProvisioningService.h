#pragma once

#include <Arduino.h>

#include "StorageService.h"
#include "RegistrationService.h"

class NimBLECharacteristic;

// ============================================================
// ProvisioningService - BLE onboarding (milestone 37)
// ============================================================
//
// Replaces the Soft-AP captive portal.
//
// UNPROVISIONED  advertising "HS-{MAC}"; configuration held in RAM only
// PROVISIONING   after COMMIT_PROVISIONING: join Wi-Fi, register with the
//                Control Server through the existing RegistrationService,
//                persist, report PROVISIONING_SUCCESS, restart
// PROVISIONED    booted from NVS; BLE is never started
//
// BLE contract (UTF-8 JSON, one message per write / indication):
//
//   Mobile -> ESP32 (Command characteristic, encrypted write)
//     {"op":"GET_IDENTITY","id":1}
//     {"op":"SET_WIFI_CONFIGURATION","id":2,"ssid":"...","password":"..."}
//     {"op":"SET_CONTROL_SERVER_CONFIGURATION","id":3,"controlServerUrl":"http://host:port"}
//     {"op":"COMMIT_PROVISIONING","id":4}
//
//   ESP32 -> Mobile (Event characteristic, indicate)
//     {"type":"IDENTITY_RESPONSE","id":1,"hardwareId":"70AF0935923C","protocolVersion":1}
//     {"type":"ACK","id":2,"ok":true}
//     {"type":"ACK","id":3,"ok":false,"code":"INVALID_CONFIGURATION"}
//     {"type":"PROVISIONING_SUCCESS","id":4,"moduleId":42}
//     {"type":"PROVISIONING_FAILED","id":4,"code":"WIFI_CONNECTION_FAILED"}
// ============================================================

class ProvisioningService
{
public:

    ProvisioningService(
        StorageService& storageService,
        RegistrationService& registrationService);

    // Provisioned: joins the stored Wi-Fi and hands the stored Control Server
    // URL to registration. Otherwise: starts BLE advertising.
    void Begin();

    void Loop();

    bool IsProvisioned() const;

    // The persisted Control Server URL; empty unless provisioned.
    const String& GetControlServerUrl() const;

    // "http://192.168.1.11:4025" -> "192.168.1.11".
    static String HostFromUrl(
        const String& url);


    // Called from NimBLE callbacks, which run on the BLE host task. They only
    // hand data to Loop().
    void OnCommandWritten(
        const uint8_t* data,
        size_t length);

    void OnClientDisconnected();

    void OnIndicationFinished();


private:

    enum class State
    {
        Unprovisioned,
        Provisioning,
        Provisioned
    };

    void StartBle();

    void StartAdvertising();

    bool HasClient() const;

    void HandleCommand(
        const String& json);

    void Send(
        const String& json);

    void SendAck(
        long id,
        bool ok);

    void Commit();

    void UpdateProvisioning();

    void Succeed();

    void Fail(
        const char* code);

    void DiscardPending();

    static bool IsValidSsid(
        const String& ssid);

    static bool IsValidPassword(
        const String& password);

    static bool IsValidUrl(
        const String& url);


    StorageService& _storageService;

    RegistrationService& _registrationService;

    State _state =
        State::Unprovisioned;

    // The persisted configuration of a provisioned board.
    ProvisioningConfig _config;

    // What the phone has sent and not yet committed. RAM only.
    ProvisioningConfig _pending;

    bool _hasWifi = false;

    bool _hasControlServer = false;

    long _commitId = 0;

    unsigned long _commitAt = 0;

    unsigned long _wifiStartedAt = 0;

    bool _wifiJoined = false;

    // Set once PROVISIONING_SUCCESS has been indicated; the board restarts
    // when the phone confirms it or after RestartDelay.
    bool _restarting = false;

    unsigned long _succeededAt = 0;

    NimBLECharacteristic* _eventCharacteristic = nullptr;


    // Written by the BLE host task, consumed by Loop().
    portMUX_TYPE _inboxLock =
        portMUX_INITIALIZER_UNLOCKED;

    static constexpr size_t MaxMessageLength = 512;

    char _inbox[MaxMessageLength + 1] = {0};

    volatile bool _hasInbox = false;

    volatile bool _clientDisconnected = false;

    volatile bool _indicationFinished = false;


    // The agreed 5-minute overall timeout, from COMMIT_PROVISIONING.
    static constexpr unsigned long ProvisioningTimeout =
        5UL * 60UL * 1000UL;

    static constexpr unsigned long WifiConnectTimeout =
        30000;

    // Registration failures tolerated before giving up (the registration task
    // retries every 10 s). A 422 fails at once.
    static constexpr int MaxRegistrationFailures = 3;

    static constexpr unsigned long RestartDelay =
        3000;
};
