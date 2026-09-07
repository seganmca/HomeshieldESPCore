#include "Debug.h"
#include "HomeShield.h"
#include <WiFi.h>

HomeShieldClass HomeShield;

HomeShieldClass* HomeShieldClass::_instance = nullptr;

void HomeShieldClass::begin(
    const String& deviceType,
    const String& firmwareVersion)
{
    _instance = this;

    _firmwareVersion = firmwareVersion;
    _deviceType = deviceType;
    _lastHeartbeat = 0;

    _registrationService =
        new RegistrationService(
            _storageService,
            _httpService,
            _deviceType);

    _provisioningService =
        new ProvisioningService(
            _storageService,
            _httpService,
            *_registrationService);

    _provisioningService->Begin();
}

void HomeShieldClass::loop()
{
    _provisioningService->Loop();

    if (!IsConnected())
        return;

    if (!_mqttInitialized)
	{
		_mqttService.begin(
			"192.168.1.11",
			1883);

		_mqttService.setCallback(OnMqttMessage);

		String topic =
			"homeshield/device/" +
			DeviceIdentity::GetHardwareId();

		_mqttService.addSubscription(topic);

		_mqttInitialized = true;

		_lastHeartbeat = millis();
	}

	_mqttService.loop();

	if (MqttConnected())
	{
		if (millis() - _lastHeartbeat >= HEARTBEAT_INTERVAL)
		{
			publishHeartbeat();

			_lastHeartbeat = millis();
		}
	}
}

void HomeShieldClass::setCommandHandler(
    CommandHandler handler)
{
    _commandHandler = handler;
}

void HomeShieldClass::OnMqttMessage(
    char* topic,
    byte* payload,
    unsigned int length)
{
    if (_instance == nullptr)
        return;

    String message;

    for (unsigned int i = 0; i < length; i++)
        message += (char)payload[i];

    if (_instance->_commandHandler)
    {
		DEBUG_VALUE("Command Received: ", message);
        _instance->_commandHandler(message);
    }
}

bool HomeShieldClass::publish(
    const String& topic,
    const String& message)
{
    if (!MqttConnected())
        return false;

    DEBUG_VALUE("Publishing event :", message);

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
    return WiFi.status() == WL_CONNECTED;
}	

void HomeShieldClass::publishHeartbeat()
{
    String request =
    "{"
        "\"hardwareId\":\"" + DeviceIdentity::GetHardwareId() + "\","
        "\"eventType\":\"Heartbeat\","
        "\"payload\":{"
            "\"firmwareVersion\":\"" + _firmwareVersion + "\""
        "}"
    "}";

    publish(
        "homeshield/events",
        request);
}

bool HomeShieldClass::MqttConnected()
{
    return _mqttInitialized && _mqttService.connected();
}