#pragma once

// ============================================================
// Module Types
// ============================================================
//
// Milestone 36. A Module is the CONTROLLER boundary; a Device is
// an independently addressable function on it. This is what kind
// of controller a board is.
//
// Mirrors HomeShield.Common/Constants/ModuleTypeKeys.cs the same
// way DeviceTypes.h mirrors DeviceTypeKeys.cs. Two declarations
// of the same string drift in silence, so if a key changes there
// it must change here.
//
// HARDWARE KEYS ONLY. The logical module types - "camera-group"
// and "system" - are created by the Control Server alone and are
// not a controller's to declare. A board that sends one is
// refused with 422.
//
// A module type is PRESENTATIONAL (decision D2): it drives card
// layout and iconography in the app and decides nothing about
// behaviour. A type this build of the Control Server does not
// know still registers and still works - it is recorded as
// "esp32-generic". That is the opposite of a device type, which
// decides capability and is refused if unknown.
//
// A single-device board does not need any of these: HomeShield's
// begin(deviceType, firmwareVersion) declares no module type and
// the Control Server derives it from the one declared device.
// ============================================================

namespace ModuleTypes
{
    // A relay controller board. The first genuinely multi-device
    // module type, and the one milestone 36 exists for.
    constexpr const char* RelayBoard    = "relay-board";

    // A HomeShield controller with no more specific type. The
    // honest fallback, not a placeholder.
    constexpr const char* Esp32Generic  = "esp32-generic";

    constexpr const char* DoorSensorNode = "door-sensor-node";

    constexpr const char* TankSensorNode = "tank-sensor-node";

    constexpr const char* SirenNode      = "siren-node";

    constexpr const char* PirNode        = "pir-node";

    constexpr const char* MmWaveNode     = "mmwave-node";

    constexpr const char* AiVisionNode   = "ai-vision-node";
}
