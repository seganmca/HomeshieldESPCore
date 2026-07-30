#include "HttpService.h"
#include "Configuration.h"
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WiFi.h>

String HttpService::Post(
    const String& url,
    const String& json)
{
    Serial.println("================================");
    Serial.println("HTTP POST");
    Serial.println(url);
    Serial.println(json);
    Serial.println("================================");

    HTTPClient client;
	WiFiClient wifiClient;

	client.setTimeout(5000);
    client.begin(wifiClient, url);
	
	Serial.print("Connected URL : ");
	Serial.println(client.getLocation());

    client.addHeader(
        "Content-Type",
        "application/json");

	Serial.println("About to POST...");
    auto status = client.POST(json);
	Serial.println("POST completed.");

    Serial.print("HTTP Status : ");
    Serial.println(status);

    String response;

    if (status > 0)
    {
        response = client.getString();

        Serial.println("Response:");
        Serial.println(response);
    }
    else
    {
        Serial.print("HTTP Error : ");
        Serial.println(client.errorToString(status));
    }

    client.end();

    return response;
}

String HttpService::GetHardwareId()
{
    String hardwareId = WiFi.macAddress();

    hardwareId.replace(":", "");
    hardwareId.toUpperCase();

    return hardwareId;
}

String HttpService::SendHeartbeat()
{
    String request =
        "{"
        "\"hardwareId\":\"" + GetHardwareId() + "\","
        "\"firmwareVersion\":\"1.0.0\""
        "}";

    return Post(
        String(Configuration::ControlServerUrl) + "/api/device/heartbeat",
        request);
}

String HttpService::PostDeviceState(int state)
{
    String request =
        "{"
        "\"hardwareId\":\"" + GetHardwareId() + "\","
        "\"state\":{"
            "\"state\":" + state +
        "}"
        "}";

    return Post(
        String(Configuration::ControlServerUrl) + "/api/device/state",
        request);
}

String HttpService::RegisterDevice(int deviceType)

{
    String request =
        "{"
        "\"hardwareId\":\"" + GetHardwareId() + "\","
        "\"deviceType\":" + String(deviceType) + ","
        "\"firmwareVersion\":\"" + String(Configuration::FirmwareVersion) + "\""
        "}";

    return Post(
        String(Configuration::ControlServerUrl) + "/api/device/register",
        request);
}

