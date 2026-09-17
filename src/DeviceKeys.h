#pragma once

// ============================================================
// Device Keys
// ============================================================
//
// Milestone 36. A DeviceKey is the module-local routing address
// of one device on a controller - "relay1", "ch2", "main". It is
// NOT an identity: Device.Id remains the canonical HomeShield
// identity, and a DeviceKey is never shown to a user and never
// stored in an automation.
//
// A multi-device sketch declares its own keys. This header holds
// the one key the LIBRARY owns.
// ============================================================

namespace DeviceKeys
{
    // ------------------------------------------------------------
    // The key of a single-device controller's only device.
    // ------------------------------------------------------------
    //
    // This exists in exactly one place on purpose, and it is the
    // most important constant in the milestone.
    //
    // The Phase 1 migration wrote DeviceKey = "main" for every
    // controller that existed before milestone 36, and the Control
    // Server writes the same value for any single device it
    // registers today. So a reflashed board MUST declare this key
    // to be matched against the child it already has: registration
    // reconciles on DeviceKey, and a board that declared anything
    // else would have a NEW Device created with a NEW Device.Id
    // while its old one was retained and disabled - silently
    // breaking every automation, alarm and history row that
    // resolves through the old id.
    //
    // That is why no sketch contains this literal.
    // HomeShield.begin(deviceType, firmwareVersion) declares it on
    // the sketch's behalf, so a single-device board cannot get its
    // key wrong, forget it, or "improve" it in one of seven places.
    constexpr const char* Main = "main";
}
