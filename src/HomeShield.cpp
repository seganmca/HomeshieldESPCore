#include "Debug.h"
#include "HomeShield.h"

#include <WiFi.h>


HomeShieldClass HomeShield;


HomeShieldClass*
HomeShieldClass::_instance =
    nullptr;


void HomeShieldClass::begin(
    const String& deviceType,
    const String& firmwareVersion)
{
    _instance = this;


    _firmwareVersion =
        firmwareVersion;


    _deviceType =
        deviceType;


    _lastHeartbeat =
        0;


    _mqttInitialized =
        false;


    _wasWifiConnected =
        false;


    _registrationService =
        new RegistrationService(
            _storageService,
            _httpService,
            _deviceType);


    _provisioningService =
        new ProvisioningService(
            _storageService);


    _provisioningService->Begin();


    // Start the registration task.
    // The task itself waits for WiFi and retries
    // until registration succeeds.
    _registrationService->Begin();
}


void HomeShieldClass::loop()
{
    /*
     * IMPORTANT:
     *
     * This function must remain fast.
     *
     * The physical device application calls this
     * function from its main loop, so no blocking
     * network operation is performed here.
     */


    if (_provisioningService != nullptr)
    {
        _provisioningService->Loop();
    }


    bool wifiConnected =
        IsConnected();


    /*
     * Detect WiFi loss.
     */
    if (!wifiConnected)
    {
        if (_wasWifiConnected)
        {
            DEBUG_LOG(
                "WiFi disconnected.");

            _mqttService.disconnect();

            _mqttInitialized =
                false;

            _wasWifiConnected =
                false;
        }

        return;
    }


    /*
     * Detect WiFi reconnection.
     */
    if (!_wasWifiConnected)
    {
        DEBUG_LOG(
            "WiFi connected.");

        _wasWifiConnected =
            true;

        _mqttInitialized =
            false;

        _lastHeartbeat =
            millis();
    }


    /*
     * Initialize MQTT once per WiFi session.
     */
    if (!_mqttInitialized)
    {
        _mqttService.begin(
            "192.168.1.11",
            1883);


        _mqttService.setCallback(
            OnMqttMessage);


        String topic =
            "homeshield/device/" +
            DeviceIdentity::GetHardwareId();


        _mqttService.addSubscription(
            topic);


        _mqttInitialized =
            true;


        _lastHeartbeat =
            millis();


        DEBUG_LOG(
            "MQTT service initialized.");
    }


    /*
     * MQTT processing is non-blocking.
     *
     * Failed connections result in a single
     * connection attempt every few seconds.
     */
    _mqttService.loop();


    /*
     * Heartbeat.
     */
    if (MqttConnected())
    {
        if (millis() -
            _lastHeartbeat >=
            HEARTBEAT_INTERVAL)
        {
            publishHeartbeat();

            _lastHeartbeat =
                millis();
        }
    }
}


void HomeShieldClass::setCommandHandler(
    CommandHandler handler)
{
    _commandHandler =
        handler;
}


void HomeShieldClass::OnMqttMessage(
    char* topic,
    byte* payload,
    unsigned int length)
{
    if (_instance == nullptr)
        return;


    String message;

    message.reserve(
        length);


    for (unsigned int i = 0;
         i < length;
         i++)
    {
        message +=
            (char)payload[i];
    }


    if (_instance->_commandHandler)
    {
        DEBUG_VALUE(
            "Command Received: ",
            message);


        _instance->_commandHandler(
            message);
    }
}


bool HomeShieldClass::publish(
    const String& topic,
    const String& message)
{
    if (!MqttConnected())
    {
        return false;
    }


    DEBUG_VALUE(
        "Publishing event :",
        message);


    return _mqttService.publish(
        topic,
        message);
}


String HomeShieldClass::GetHardwareId()
{
    return DeviceIdentity::GetHardwareId();
}


bool HomeShieldClass::IsConnected() const
{
    return WiFi.status() ==
        WL_CONNECTED;
}


void HomeShieldClass::publishHeartbeat()
{
    String request =
        "{"
        "\"hardwareId\":\"" +
        DeviceIdentity::GetHardwareId() +
        "\","
        "\"eventType\":\"Heartbeat\","
        "\"payload\":{"
        "\"firmwareVersion\":\"" +
        _firmwareVersion +
        "\""
        "}"
        "}";


    publish(
        "homeshield/events",
        request);
}


bool HomeShieldClass::MqttConnected()
{
    return
        _mqttInitialized &&
        _mqttService.connected();
}