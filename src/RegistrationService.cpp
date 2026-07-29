#include "RegistrationService.h"
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

    auto response = _httpService.RegisterDevice(_deviceType);

    Serial.println(response);
}