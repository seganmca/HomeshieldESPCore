#pragma once

#include <Arduino.h>

class HttpService
{
public:

    String Post(
        const String& url,
        const String& json);
		
	String PostDeviceState(int state);
	String SendHeartbeat();
	String RegisterDevice(int deviceType);
	
private:	
	String GetHardwareId();
	
};