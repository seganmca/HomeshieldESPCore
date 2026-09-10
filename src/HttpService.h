#pragma once

#include <Arduino.h>
#include <DeviceIdentity.h>

struct HttpResult
{
    int statusCode = 0;
    String response;
    bool success = false;
};


class HttpService
{
public:

    HttpResult Post(
        const String& url,
        const String& json);


    String PostDeviceState(
        int state);


    String SendHeartbeat();


    HttpResult RegisterDevice(
        const String& deviceType);


    String GetHardwareId();
};