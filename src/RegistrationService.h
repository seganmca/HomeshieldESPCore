#pragma once

#include "HttpService.h"
#include "StorageService.h"

#include <Arduino.h>

class RegistrationService
{
public:

    RegistrationService(
        StorageService& storageService,
        HttpService& httpService,
        const String& deviceType);

    ~RegistrationService();

    void Begin();

    void Loop();

    bool IsRegistered() const;

    void Reset();

private:

    static void RegistrationTaskEntry(
        void* parameter);

    void RegistrationTask();

    void RegisterOnce();


    StorageService& _storageService;
    HttpService& _httpService;

    String _deviceType;


    volatile bool _registered = false;

    volatile bool _taskRunning = false;

    TaskHandle_t _taskHandle = nullptr;


    static constexpr unsigned long RegistrationRetryInterval =
        10000;

    static constexpr uint32_t RegistrationTaskStackSize =
        8192;

    static constexpr UBaseType_t RegistrationTaskPriority =
        1;
};