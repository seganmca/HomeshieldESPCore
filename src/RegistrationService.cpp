#include "RegistrationService.h"
#include <Arduino.h>
#include <WiFi.h>
#include "Debug.h"

RegistrationService::RegistrationService(
    StorageService& storageService,
    HttpService& httpService,
    const String& deviceType)
    : _storageService(storageService),
      _httpService(httpService),
      _deviceType(deviceType)
{
}

void RegistrationService::Register()
{
    DEBUG_LOG("Registering device...");

    auto response = _httpService.RegisterDevice(_deviceType);

    DEBUG_LOG(response);
}