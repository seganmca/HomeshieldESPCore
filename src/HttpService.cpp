#include "Debug.h"
#include "HttpService.h"
#include "Configuration.h"

#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WiFi.h>


HttpResult HttpService::Post(
    const String& url,
    const String& json)
{
    HttpResult result;


    if (WiFi.status() !=
        WL_CONNECTED)
    {
        DEBUG_LOG(
            "HTTP request skipped: WiFi not connected.");

        return result;
    }


    DEBUG_LOG(url);
    DEBUG_LOG(json);


    HTTPClient client;
    WiFiClient wifiClient;


    client.setConnectTimeout(
        3000);

    client.setTimeout(
        5000);


    if (!client.begin(
            wifiClient,
            url))
    {
        DEBUG_LOG(
            "HTTP begin failed.");

        return result;
    }


    client.addHeader(
        "Content-Type",
        "application/json");


    auto status =
        client.POST(json);


    DEBUG_LOG(
        "POST completed.");


    DEBUG_VALUE(
        "HTTP Status : ",
        status);


    result.statusCode =
        status;


    if (status > 0)
    {
        result.response =
            client.getString();

        DEBUG_VALUE(
            "Response:",
            result.response);


        result.success =
            status >= 200 &&
            status < 300;
    }
    else
    {
        DEBUG_VALUE(
            "HTTP Error : ",
            client.errorToString(status));
    }


    client.end();


    return result;
}


HttpResult HttpService::RegisterDevice(
    const String& deviceType)
{
    String request =
        "{"
        "\"hardwareId\":\"" +
        DeviceIdentity::GetHardwareId() +
        "\","
        "\"deviceType\":\"" +
        deviceType +
        "\","
        "\"firmwareVersion\":\"" +
        String(Configuration::FirmwareVersion) +
        "\""
        "}";


    return Post(
        String(Configuration::ControlServerUrl) +
            "/api/device/register",
        request);
}


String HttpService::PostDeviceState(
    int state)
{
    String request =
        "{"
        "\"hardwareId\":\"" +
        DeviceIdentity::GetHardwareId() +
        "\","
        "\"state\":" +
        String(state) +
        "}";


    auto result =
        Post(
            String(Configuration::ControlServerUrl) +
                "/api/device/state",
            request);


    return result.response;
}


String HttpService::SendHeartbeat()
{
    String request =
        "{"
        "\"hardwareId\":\"" +
        DeviceIdentity::GetHardwareId() +
        "\""
        "}";


    auto result =
        Post(
            String(Configuration::ControlServerUrl) +
                "/api/device/heartbeat",
            request);


    return result.response;
}


String HttpService::GetHardwareId()
{
    return DeviceIdentity::GetHardwareId();
}