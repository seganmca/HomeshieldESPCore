#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DeviceIdentity.h>

class MqttService
{
public:
    MqttService();

    void begin(
        const char* server,
        int port);

    void loop();

    bool publish(
        const String& topic,
        const String& message);

    // qos is the SUBSCRIPTION quality of service. 0 for everything that
    // existed before milestone 40, which is what the default preserves; 1 for
    // a topic whose one message is sent once and never repeated.
    void addSubscription(
        const String& topic,
        uint8_t qos = 0);

    void setCallback(
        MQTT_CALLBACK_SIGNATURE);

    bool connected();

    void disconnect();

private:
    void connect();

    WiFiClient _wifiClient;
    PubSubClient _mqttClient;

    const char* _server = nullptr;
    int _port = 1883;

    String _clientId;

    static constexpr int MAX_SUBSCRIPTIONS = 10;

    String _subscriptions[
        MAX_SUBSCRIPTIONS];

    uint8_t _subscriptionQos[
        MAX_SUBSCRIPTIONS] = {0};

    int _subscriptionCount = 0;

    unsigned long _lastConnectAttempt = 0;

    static constexpr unsigned long MQTT_RETRY_INTERVAL =
        5000;
};