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
                _subscriptions[i].c_str(),
                _subscriptionQos[i]);
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
    const String& topic,
    uint8_t qos)
{
    if (topic.length() == 0)
        return;


    // --------------------------------------------------
    // Idempotent, and that is a fix rather than a nicety
    // --------------------------------------------------
    //
    // HomeShieldClass re-subscribes on every Wi-Fi session: _mqttInitialized is
    // cleared when the link drops and the whole initialisation block runs again
    // when it returns. Before milestone 40 that appended a duplicate entry per
    // reconnect, so a board that had lost Wi-Fi ten times filled the table and
    // silently stopped subscribing to anything - including its own command
    // topic.
    //
    // M40 makes that worse by adding a second topic, halving the number of
    // reconnects a board survives, so it is dealt with here where the table is
    // owned. An already-registered topic is re-subscribed on the broker (the
    // session may be new) but does not take a second slot.
    for (int i = 0; i < _subscriptionCount; i++)
    {
        if (_subscriptions[i] != topic)
            continue;

        _subscriptionQos[i] = qos;

        if (_mqttClient.connected())
        {
            _mqttClient.subscribe(
                topic.c_str(),
                qos);
        }

        return;
    }


    if (_subscriptionCount >=
        MAX_SUBSCRIPTIONS)
    {
        DEBUG_LOG(
            "MQTT subscription limit reached.");

        return;
    }


    _subscriptionQos[
        _subscriptionCount] = qos;

    _subscriptions[
        _subscriptionCount++] =
            topic;


    if (_mqttClient.connected())
    {
        _mqttClient.subscribe(
            topic.c_str(),
            qos);
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