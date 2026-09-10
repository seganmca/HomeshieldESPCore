#include "RegistrationService.h"

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


RegistrationService::~RegistrationService()
{
    if (_taskHandle != nullptr)
    {
        vTaskDelete(
            _taskHandle);

        _taskHandle = nullptr;
    }

    _taskRunning = false;
}


void RegistrationService::Begin()
{
    if (_taskHandle != nullptr)
        return;


    BaseType_t result =
        xTaskCreate(
            RegistrationTaskEntry,
            "HSRegistration",
            RegistrationTaskStackSize,
            this,
            RegistrationTaskPriority,
            &_taskHandle);


    if (result != pdPASS)
    {
        _taskHandle = nullptr;

        DEBUG_LOG(
            "Failed to create registration task.");
    }
    else
    {
        DEBUG_LOG(
            "Registration task started.");
    }
}


void RegistrationService::Loop()
{
    // Registration runs independently in its own
    // FreeRTOS task. Nothing blocking happens here.
}


void RegistrationService::RegistrationTaskEntry(
    void* parameter)
{
    auto* service =
        static_cast<RegistrationService*>(
            parameter);


    if (service == nullptr)
    {
        vTaskDelete(nullptr);
        return;
    }


    service->RegistrationTask();


    service->_taskRunning = false;
    service->_taskHandle = nullptr;


    vTaskDelete(nullptr);
}


void RegistrationService::RegistrationTask()
{
    _taskRunning = true;


    for (;;)
    {
        if (WiFi.status() !=
            WL_CONNECTED)
        {
            _registered = false;

            vTaskDelay(
                pdMS_TO_TICKS(1000));

            continue;
        }


        if (!_registered)
        {
            RegisterOnce();
        }


        if (_registered)
        {
            vTaskDelay(
                pdMS_TO_TICKS(1000));
        }
        else
        {
            vTaskDelay(
                pdMS_TO_TICKS(
                    RegistrationRetryInterval));
        }
    }
}


void RegistrationService::RegisterOnce()
{
    if (WiFi.status() !=
        WL_CONNECTED)
    {
        return;
    }


    DEBUG_LOG(
        "Registering device...");


    auto result =
        _httpService.RegisterDevice(
            _deviceType);


    if (result.response.length() > 0)
    {
        DEBUG_LOG(
            result.response);
    }


    if (result.success)
    {
        _registered = true;

        DEBUG_VALUE(
            "Device registration successful. HTTP=",
            result.statusCode);
    }
    else
    {
        _registered = false;

        DEBUG_VALUE(
            "Device registration failed. HTTP=",
            result.statusCode);
    }
}


bool RegistrationService::IsRegistered() const
{
    return _registered;
}


void RegistrationService::Reset()
{
    _registered = false;
}