#include "RegistrationService.h"
#include "Configuration.h"
#include <Arduino.h>
#include <WiFi.h>

RegistrationService::RegistrationService(
    StorageService& storageService,
    HttpService& httpService,
    int deviceType)
    : _storageService(storageService),
      _httpService(httpService),
      _deviceType(deviceType)
{
}

void RegistrationService::Register()
{
    Serial.println("Registering device...");

	String hardwareId =
		WiFi.macAddress();

	hardwareId.replace(":", "");
	hardwareId.toUpperCase();

	String request =
		"{"
		"\"hardwareId\":\"" + hardwareId + "\","
		 "\"deviceType\":" + String(_deviceType) + ","
		"\"firmwareVersion\":\"1.0.0\""
		"}";

    auto response =
        _httpService.Post(
            String(Configuration::ControlServerUrl) + "/api/device/register",
            request);


    Serial.println(response);
}