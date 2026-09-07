#pragma once

#include <Arduino.h>
#include <DeviceIdentity.h>

class HttpService
{
public:

    String Post(
        const String& url,
        const String& json);
		
	String PostDeviceState(int state);
	String SendHeartbeat();
	String RegisterDevice(const String& deviceType);
	String GetHardwareId();
	
};