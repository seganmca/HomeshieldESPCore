#include "Debug.h"
#include "HttpService.h"
#include "Configuration.h"
#include "JsonLite.h"

#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WiFi.h>


HttpResult HttpService::Post(
    const String& url,
    const String& json)
{
    HttpResult result;


    // Milestone 37. No compiled-in fallback: without an onboarded Control
    // Server URL there is nowhere to post.
    if (!url.startsWith("http://"))
    {
        DEBUG_LOG(
            "HTTP request skipped: no Control Server URL.");

        return result;
    }


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


HttpResult HttpService::RegisterModule(
    const String& controlServerUrl,
    const String& moduleType,
    const String& firmwareVersion,
    const DeclaredDevice* devices,
    int deviceCount)
{
    String request =
        "{"
        "\"hardwareId\":\"" +
        DeviceIdentity::GetHardwareId() +
        "\",";


    // Omitted when empty. A single-device board declares no module
    // type and the Control Server derives it from the one device.
    if (moduleType.length() > 0)
    {
        request +=
            "\"moduleType\":\"" +
            JsonLite::Escape(moduleType) +
            "\",";
    }


    request +=
        "\"firmwareVersion\":\"" +
        JsonLite::Escape(firmwareVersion) +
        "\","
        "\"devices\":[";


    for (int i = 0; i < deviceCount; i++)
    {
        if (i > 0) request += ",";

        request +=
            "{\"deviceKey\":\"" +
            JsonLite::Escape(devices[i].deviceKey) +
            "\",\"deviceType\":\"" +
            JsonLite::Escape(devices[i].deviceType) +
            "\"";


        if (devices[i].defaultName.length() > 0)
        {
            request +=
                ",\"defaultName\":\"" +
                JsonLite::Escape(devices[i].defaultName) +
                "\"";
        }


        request += "}";
    }


    request += "]}";


    return Post(
        controlServerUrl +
            "/api/module/register",
        request);
}


HttpResult HttpService::RegisterDevice(
    const String& controlServerUrl,
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
        controlServerUrl +
            "/api/device/register",
        request);
}


String HttpService::PostDeviceState(
    const String& controlServerUrl,
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
            controlServerUrl +
                "/api/device/state",
            request);


    return result.response;
}


String HttpService::SendHeartbeat(
    const String& controlServerUrl)
{
    String request =
        "{"
        "\"hardwareId\":\"" +
        DeviceIdentity::GetHardwareId() +
        "\""
        "}";


    auto result =
        Post(
            controlServerUrl +
                "/api/device/heartbeat",
            request);


    return result.response;
}


String HttpService::GetHardwareId()
{
    return DeviceIdentity::GetHardwareId();
}
