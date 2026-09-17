#pragma once

#include <Arduino.h>
#include <DeviceIdentity.h>
#include <DeclaredDevice.h>

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


    // --------------------------------------------------
    // Module registration (v2)
    // --------------------------------------------------
    //
    // Milestone 36 phase 3. POSTs to api/module/register:
    //
    //   {
    //     "hardwareId": "A4CF12AB34CD",
    //     "moduleType": "relay-board",
    //     "firmwareVersion": "2.0.0",
    //     "devices": [
    //       { "deviceKey": "relay1", "deviceType": "switch",
    //         "defaultName": "Channel 1" }
    //     ]
    //   }
    //
    // moduleType is OMITTED when empty, which is what a
    // single-device board sends: with exactly one declared device
    // the Control Server derives the module type, so a reflashed
    // door sensor keeps the module type it already had instead of
    // being demoted to generic.
    //
    // defaultName is omitted per device when empty.
    //
    // firmwareVersion is a PARAMETER rather than
    // Configuration::FirmwareVersion. The old RegisterDevice read
    // the compiled-in constant while the heartbeat reported the
    // value passed to HomeShield.begin(), so the two could
    // disagree about the same board - they only ever matched
    // because every sketch happened to pass "1.0.0".
    //
    // Milestone 37: controlServerUrl is a parameter. It is no longer compiled
    // in; RegistrationService holds the URL onboarding delivered.
    HttpResult RegisterModule(
        const String& controlServerUrl,
        const String& moduleType,
        const String& firmwareVersion,
        const DeclaredDevice* devices,
        int deviceCount);


    // --------------------------------------------------
    // Device registration (v1)
    // --------------------------------------------------
    //
    // RETAINED AND UNUSED. Nothing in this library calls it after
    // milestone 36 phase 3; the Control Server keeps
    // api/device/register for boards that have not been reflashed
    // and for cameras it provisions itself.
    //
    // Kept so the v1 body is still documented in one place rather
    // than only in the server's contract. Do not reach for it: it
    // declares a DEVICE, not a module, and the response cannot
    // describe more than one child.
    HttpResult RegisterDevice(
        const String& controlServerUrl,
        const String& deviceType);


    String PostDeviceState(
        const String& controlServerUrl,
        int state);


    String SendHeartbeat(
        const String& controlServerUrl);


    String GetHardwareId();


};
