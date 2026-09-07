#pragma once

#include <DeviceIdentity.h>
#include <StorageService.h>
#include <HttpService.h>
#include <RegistrationService.h>
#include <ProvisioningService.h>
#include <MqttService.h>


typedef void (*CommandHandler)(const String&);


class HomeShieldClass
{
public:

    void begin(const String& deviceType, const String& firmwareVersion);

    void loop();

    void setCommandHandler(CommandHandler handler);

    bool publish(
        const String& topic,
        const String& message);

    String GetHardwareId();

    bool IsConnected() const;

    bool MqttConnected();

private:

    unsigned long _lastHeartbeat;

    static constexpr unsigned long HEARTBEAT_INTERVAL = 60 * 1000;

    String _firmwareVersion;

    StorageService _storageService;

    HttpService _httpService;

    RegistrationService* _registrationService = nullptr;

    ProvisioningService* _provisioningService = nullptr;

    MqttService _mqttService;

    CommandHandler _commandHandler = nullptr;

    bool _mqttInitialized = false;

    String _deviceType;

    static HomeShieldClass* _instance;

    static void OnMqttMessage(
        char* topic,
        byte* payload,
        unsigned int length);

    void publishHeartbeat();
};


extern HomeShieldClass HomeShield;