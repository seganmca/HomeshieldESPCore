#include "RegistrationService.h"

#include <WiFi.h>

#include "Debug.h"
#include "JsonLite.h"


RegistrationService::RegistrationService(
    StorageService& storageService,
    HttpService& httpService,
    const String& moduleType,
    const String& firmwareVersion,
    const DeclaredDevice* devices,
    int deviceCount)
    : _storageService(storageService),
      _httpService(httpService),
      _moduleType(moduleType),
      _firmwareVersion(firmwareVersion),
      _devices(devices),
      _deviceCount(deviceCount)
{
    _urlLock =
        xSemaphoreCreateMutex();
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
    // Registration runs independently in its own FreeRTOS task.
    // Nothing blocking happens here.
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


        bool attempted = false;

        if (!_registered)
        {
            attempted = RegisterOnce();
        }


        // Milestone 37: an idle task (no Control Server URL yet) checks
        // again in a second, so a freshly onboarded URL is used promptly.
        if (_registered || !attempted)
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


bool RegistrationService::RegisterOnce()
{
    if (WiFi.status() !=
        WL_CONNECTED)
    {
        return false;
    }


    // Nothing to declare means nothing to register. HomeShieldClass
    // refuses to construct this service in that state, so reaching
    // here would be a bug rather than a field condition.
    if (_devices == nullptr ||
        _deviceCount <= 0)
    {
        return false;
    }


    String controlServerUrl;

    xSemaphoreTake(_urlLock, portMAX_DELAY);
    controlServerUrl = _controlServerUrl;
    xSemaphoreGive(_urlLock);


    // Not onboarded yet, or a provisioning attempt was abandoned. Nothing to
    // register with.
    if (controlServerUrl.length() == 0)
    {
        return false;
    }


    DEBUG_LOG(
        "Registering module...");


    auto result =
        _httpService.RegisterModule(
            controlServerUrl,
            _moduleType,
            _firmwareVersion,
            _devices,
            _deviceCount);


    if (result.response.length() > 0)
    {
        DEBUG_LOG(
            result.response);
    }


    _lastStatusCode =
        result.statusCode;


    if (result.success)
    {
        long moduleId = 0;

        JsonLite::ReadLong(
            result.response,
            "moduleId",
            moduleId);

        _moduleId = moduleId;

        _failedAttempts = 0;

        _registered = true;

        DEBUG_VALUE(
            "Module registration successful. HTTP=",
            result.statusCode);
    }
    else
    {
        _failedAttempts++;

        _registered = false;

        // A 422 is the Control Server refusing the DECLARATION
        // itself - an unknown device type, a malformed device key, a
        // duplicate key, or a multi-device module with no module
        // type. Retrying will not help until the sketch is fixed, so
        // it is called out rather than buried in the retry loop.
        if (result.statusCode == 422)
        {
            Serial.println(
                "[HomeShield] Module registration REFUSED (HTTP 422). The "
                "declaration is wrong, not the connection - check the device "
                "types, the device keys, and that a multi-device board declares "
                "its module type. Retrying will not help until the sketch is "
                "corrected.");

            Serial.println(
                result.response);
        }
        else
        {
            DEBUG_VALUE(
                "Module registration failed. HTTP=",
                result.statusCode);
        }
    }

    return true;
}


bool RegistrationService::IsRegistered() const
{
    return _registered;
}


void RegistrationService::Reset()
{
    _registered = false;
}


void RegistrationService::SetControlServerUrl(
    const String& url)
{
    xSemaphoreTake(_urlLock, portMAX_DELAY);
    _controlServerUrl = url;
    xSemaphoreGive(_urlLock);

    _failedAttempts = 0;
    _lastStatusCode = 0;
    _moduleId = 0;
    _registered = false;
}


int RegistrationService::FailedAttempts() const
{
    return _failedAttempts;
}


int RegistrationService::LastStatusCode() const
{
    return _lastStatusCode;
}


long RegistrationService::ModuleId() const
{
    return _moduleId;
}
