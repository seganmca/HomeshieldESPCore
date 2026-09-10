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

    _clientId =
        DeviceIdentity::GetHardwareId();

    _mqttClient.setServer(
        server,
        port);

    _lastConnectAttempt = 0;
}


void MqttService::setCallback(
    MQTT_CALLBACK_SIGNATURE)
{
    _mqttClient.setCallback(
        callback);
}


void MqttService::loop()
{
    if (WiFi.status() !=
        WL_CONNECTED)
    {
        if (_mqttClient.connected())
        {
            _mqttClient.disconnect();
        }

        return;
    }


    if (!_mqttClient.connected())
    {
        auto now =
            millis();

        if (now -
            _lastConnectAttempt >=
            MQTT_RETRY_INTERVAL)
        {
            _lastConnectAttempt =
                now;

            connect();
        }

        return;
    }


    _mqttClient.loop();
}


void MqttService::connect()
{
    if (WiFi.status() !=
        WL_CONNECTED)
    {
        return;
    }


    if (_mqttClient.connected())
    {
        return;
    }


    DEBUG_LOG_PRINT(
        "Connecting MQTT... ");


    if (_mqttClient.connect(
            _clientId.c_str()))
    {
        DEBUG_LOG(
            "SUCCESS");


        for (int i = 0;
             i < _subscriptionCount;
             i++)
        {
            if (_subscriptions[i].length() == 0)
                continue;

            _mqttClient.subscribe(
                _subscriptions[i].c_str());
        }
    }
    else
    {
        DEBUG_VALUE(
            "FAILED. State=",
            _mqttClient.state());
    }
}


bool MqttService::publish(
    const String& topic,
    const String& message)
{
    if (WiFi.status() !=
        WL_CONNECTED)
    {
        return false;
    }


    if (!_mqttClient.connected())
    {
        return false;
    }


    return _mqttClient.publish(
        topic.c_str(),
        message.c_str());
}


void MqttService::addSubscription(
    const String& topic)
{
    if (topic.length() == 0)
        return;


    if (_subscriptionCount >=
        MAX_SUBSCRIPTIONS)
    {
        DEBUG_LOG(
            "MQTT subscription limit reached.");

        return;
    }


    _subscriptions[
        _subscriptionCount++] =
            topic;


    if (_mqttClient.connected())
    {
        _mqttClient.subscribe(
            topic.c_str());
    }
}


bool MqttService::connected()
{
    return
        WiFi.status() == WL_CONNECTED &&
        _mqttClient.connected();
}


void MqttService::disconnect()
{
    if (_mqttClient.connected())
    {
        _mqttClient.disconnect();
    }
}