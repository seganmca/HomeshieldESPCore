#pragma once

#include <Arduino.h>

// ============================================================
// Declared Device
// ============================================================
//
// Milestone 36. One device a controller says it has.
//
// The declaration is authoritative: there is no discovery
// protocol and none is proposed. A module declares its children
// at registration and that declaration is the truth - with the
// one caveat that a child the module stops declaring is retained
// and DISABLED by the Control Server, never deleted, because its
// Device.Id is what automations and history resolve through.
//
// Lives in its own header rather than inside HomeShieldClass
// because RegistrationService and HttpService both need to read
// the declared list to build the registration body.
// ============================================================

struct DeclaredDevice
{
    // The module-local routing address. Lowercase letters,
    // digits, '-' and '_'; at most 32 characters.
    String deviceKey;

    // A canonical device type key from DeviceTypes.h.
    String deviceType;

    // Optional. A name for this device until the household gives
    // it one. It matters on a multi-device board: without it every
    // channel of a relay board would capture the same Device Model
    // name and be indistinguishable in the app.
    String defaultName;
};
