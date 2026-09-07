#include "MqttService.h"
#include "Debug.h"

MqttService::MqttService()
    : _mqttClient(_wifiClient)
{
}

void MqttService::begin(
    const char* server,
    int port)
{
    _server = server;
    _port = port;

    _clientId = DeviceIdentity::GetHardwareId();

    _mqttClient.setServer(server, port);
}

void MqttService::setCallback(MQTT_CALLBACK_SIGNATURE)
{
    _mqttClient.setCallback(callback);
}

void MqttService::loop()
{
    if (!_mqttClient.connected())
    {
        connect();
    }

    _mqttClient.loop();
}

void MqttService::connect()
{
    while (!_mqttClient.connected())
    {
        DEBUG_LOG_PRINT("Connecting MQTT... ");

        if (_mqttClient.connect(_clientId.c_str()))
        {
            DEBUG_LOG("SUCCESS");

            for (int i = 0; i < _subscriptionCount; i++)
            {
                _mqttClient.subscribe(_subscriptions[i].c_str());
            }
        }
        else
        {
            DEBUG_VALUE("FAILED. State=", _mqttClient.state());

            delay(2000);
        }
    }
}

bool MqttService::publish(
    const String& topic,
    const String& message)
{
	if (!_mqttClient.connected())
	{
		connect();
	}

	if (!_mqttClient.connected())
		return false;


    return _mqttClient.publish(
        topic.c_str(),
        message.c_str());
}

void MqttService::addSubscription(const String& topic)
{
    if (_subscriptionCount >= MAX_SUBSCRIPTIONS)
        return;

    _subscriptions[_subscriptionCount++] = topic;

    if (_mqttClient.connected())
    {
        _mqttClient.subscribe(topic.c_str());
    }
}

bool MqttService::connected()
{
    return _mqttClient.connected();
}