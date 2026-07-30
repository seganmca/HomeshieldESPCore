#include "MqttService.h"

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

    _clientId = GetHardwareId();

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
        Serial.print("Connecting MQTT... ");

        if (_mqttClient.connect(_clientId.c_str()))
        {
            Serial.println("SUCCESS");

            for (int i = 0; i < _subscriptionCount; i++)
            {
                _mqttClient.subscribe(_subscriptions[i].c_str());
            }
        }
        else
        {
            Serial.print("FAILED. State=");
            Serial.println(_mqttClient.state());

            delay(2000);
        }
    }
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

String MqttService::GetHardwareId()
{
    String hardwareId = WiFi.macAddress();

    hardwareId.replace(":", "");
    hardwareId.toUpperCase();

    return hardwareId;
}