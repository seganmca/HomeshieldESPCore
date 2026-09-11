#pragma once

#include <Arduino.h>
#include <DeviceIdentity.h>
#include <StorageService.h>
#include <HttpService.h>
#include <RegistrationService.h>
#include <ProvisioningService.h>
#include <MqttService.h>


typedef void (*CommandHandler)(
    const String&);


// --------------------------------------------------
// Device Telemetry
// --------------------------------------------------
//
// batteryLevel:
//   0-100 = valid battery percentage
//   -1    = not available
//
// wifiStrength:
//   WiFi RSSI in dBm
//   Example: -45, -62, -78
//
// WiFi strength is collected automatically by
// HomeShield. The device only needs to provide
// battery level if it has a battery.
// --------------------------------------------------

struct DeviceTelemetry
{
    int batteryLevel = -1;
};


class HomeShieldClass
{
public:

    void begin(
        const String& deviceType,
        const String& firmwareVersion);

    void loop();


    void setCommandHandler(
        CommandHandler handler);


    bool publish(
        const String& topic,
        const String& message);


    String GetHardwareId();


    bool IsConnected() const;


    bool MqttConnected();


    // --------------------------------------------------
    // Telemetry
    // --------------------------------------------------

    void SetBatteryLevel(
        int batteryLevel);


private:

    unsigned long _lastHeartbeat = 0;


    static constexpr unsigned long HEARTBEAT_INTERVAL =
        60 * 1000;


    String _firmwareVersion;

    String _deviceType;


    StorageService _storageService;


    HttpService _httpService;


    RegistrationService*
        _registrationService = nullptr;


    ProvisioningService*
        _provisioningService = nullptr;


    MqttService _mqttService;


    CommandHandler
        _commandHandler = nullptr;


    bool _mqttInitialized = false;

    bool _wasWifiConnected = false;


    DeviceTelemetry _telemetry;


    static HomeShieldClass*
        _instance;


    static void OnMqttMessage(
        char* topic,
        byte* payload,
        unsigned int length);


    void publishHeartbeat();
};


extern HomeShieldClass HomeShield;