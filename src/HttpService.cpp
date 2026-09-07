#include "Debug.h"
#include "HttpService.h"
#include "Configuration.h"
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WiFi.h>

String HttpService::Post(
    const String& url,
    const String& json)
{
    DEBUG_LOG(url);
    DEBUG_LOG(json);

    HTTPClient client;
	WiFiClient wifiClient;

	client.setTimeout(5000);
    client.begin(wifiClient, url);

    client.addHeader(
        "Content-Type",
        "application/json");

    auto status = client.POST(json);
	DEBUG_LOG("POST completed.");

    DEBUG_VALUE("HTTP Status : ", status);

    String response;

    if (status > 0)
    {
        response = client.getString();

        DEBUG_VALUE("Response:", response);
    }
    else
    {
        DEBUG_VALUE("HTTP Error : ", client.errorToString(status));
    }

    client.end();

    return response;
}

String HttpService::RegisterDevice(const String& deviceType)

{
    String request =
        "{"
        "\"hardwareId\":\"" + DeviceIdentity::GetHardwareId() + "\","
        "\"deviceType\":\"" + deviceType + "\","
        "\"firmwareVersion\":\"" + String(Configuration::FirmwareVersion) + "\""
        "}";

    return Post(
        String(Configuration::ControlServerUrl) + "/api/device/register",
        request);
}

