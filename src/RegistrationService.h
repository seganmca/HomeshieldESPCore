#pragma once

#include "HttpService.h"
#include "StorageService.h"
#include "DeclaredDevice.h"

#include <Arduino.h>

class RegistrationService
{
public:

    // Milestone 36 phase 3. A MODULE registers, declaring the
    // devices it holds, so this takes the module's type and the
    // declared list rather than one device type.
    //
    // moduleType may be empty: a single-device board declares none
    // and the Control Server derives it.
    //
    // The devices pointer is NOT owned and is not copied. It points
    // at HomeShieldClass's declared array, which lives for the
    // lifetime of the program - the declaration is complete before
    // this service is constructed and never changes afterwards.
    //
    // Milestone 38: capabilities is the same arrangement - a pointer into
    // HomeShieldClass's array, not owned and not copied. It is declared before
    // the module starts and never changes afterwards, so unlike the device
    // count it needs no re-read path.
    RegistrationService(
        StorageService& storageService,
        HttpService& httpService,
        const String& moduleType,
        const String& firmwareVersion,
        const DeclaredDevice* devices,
        int deviceCount,
        const String* capabilities = nullptr,
        int capabilityCount = 0);

    ~RegistrationService();

    void Begin();

    void Loop();

    bool IsRegistered() const;

    void Reset();


    // --------------------------------------------------
    // Milestone 37
    // --------------------------------------------------
    //
    // The Control Server URL to register with. It is no longer compiled in:
    // it comes from NVS on a provisioned board, or from BLE onboarding while
    // provisioning. With an empty URL the task idles.
    //
    // Setting it clears the registered flag and the attempt counters, so a
    // provisioning attempt reads only its own outcome.
    void SetControlServerUrl(
        const String& url);

    // Consecutive failed attempts since the URL was set or the last success.
    int FailedAttempts() const;

    // HTTP status of the last attempt; 0 or negative when no response arrived.
    int LastStatusCode() const;

    // Module.Id from the last successful registration response, or 0.
    long ModuleId() const;


    // --------------------------------------------------
    // Milestone 38
    // --------------------------------------------------
    //
    // Re-reads the declared array - the SAME pointer, a new count - and
    // registers again.
    //
    // This exists because a Sensor Hub gains a child at runtime, which nothing
    // before M38 could do: the declared array's ADDRESS never moves, but its
    // count was copied by value here at construction, so appending to it
    // upstream would otherwise be invisible to this service.
    //
    // Takes _urlLock while it writes the count, so the registration task can
    // never read a torn value, and clears _registered and the attempt counters
    // so the caller reads only its own outcome - exactly what
    // SetControlServerUrl does for onboarding.
    //
    // The registration task picks this up within a second and POSTs the WHOLE
    // declaration: every persisted node plus the new one. There is no
    // incremental "add one child" call, and there must not be - the Control
    // Server treats a registration as the complete truth about a module's
    // children and disables any it stops hearing about.
    void Redeclare(
        int deviceCount);

private:

    static void RegistrationTaskEntry(
        void* parameter);

    void RegistrationTask();

    // True when an HTTP attempt was made.
    bool RegisterOnce();


    StorageService& _storageService;
    HttpService& _httpService;

    String _moduleType;
    String _firmwareVersion;

    const DeclaredDevice* _devices = nullptr;

    // Guarded by _urlLock from milestone 38 onward, because Redeclare() writes
    // it from the application task while the registration task reads it.
    volatile int _deviceCount = 0;

    const String* _capabilities = nullptr;
    int _capabilityCount = 0;


    // Guards _controlServerUrl. The loop sets it; the task reads it.
    SemaphoreHandle_t _urlLock = nullptr;

    String _controlServerUrl;

    volatile bool _registered = false;

    volatile int _failedAttempts = 0;

    volatile int _lastStatusCode = 0;

    volatile long _moduleId = 0;

    volatile bool _taskRunning = false;

    TaskHandle_t _taskHandle = nullptr;


    static constexpr unsigned long RegistrationRetryInterval =
        10000;

    static constexpr uint32_t RegistrationTaskStackSize =
        8192;

    static constexpr UBaseType_t RegistrationTaskPriority =
        1;
};
