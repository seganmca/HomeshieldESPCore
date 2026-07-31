#include "HttpService.h"
#include "Configuration.h"
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WiFi.h>

String HttpService::Post(
    const String& url,
    const String& json)
{
    //Serial.println("================================");
    //Serial.println("HTTP POST");
    Serial.println(url);
    Serial.println(json);
    //Serial.println("================================");

    HTTPClient client;
	WiFiClient wifiClient;

	client.setTimeout(5000);
    client.begin(wifiClient, url);

    client.addHeader(
        "Content-Type",
        "application/json");

	//Serial.println("About to POST...");
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

String HttpService::RegisterDevice(int deviceType)

{
    String request =
        "{"
        "\"hardwareId\":\"" + DeviceIdentity::GetHardwareId() + "\","
        "\"deviceType\":" + String(deviceType) + ","
        "\"firmwareVersion\":\"" + String(Configuration::FirmwareVersion) + "\""
        "}";

    return Post(
        String(Configuration::ControlServerUrl) + "/api/device/register",
        request);
}

