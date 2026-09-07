#pragma once

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

	bool publish( const String& topic,  const String& message);

    void addSubscription(const String& topic);

    void setCallback(MQTT_CALLBACK_SIGNATURE);

	bool connected();

private:
    void connect();

private:
    WiFiClient _wifiClient;
    PubSubClient _mqttClient;

    const char* _server = nullptr;
    int _port = 1883;

    String _clientId;

    static constexpr int MAX_SUBSCRIPTIONS = 10;

    String _subscriptions[MAX_SUBSCRIPTIONS];
    int _subscriptionCount = 0;
};