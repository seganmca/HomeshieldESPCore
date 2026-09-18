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
// know still registers and still works - since milestone 39 it is
// recorded as "mono-device" rather than "esp32-generic". That is
// the opposite of a device type, which decides capability and is
// refused if unknown.
//
// A single-device board does not need any of these: HomeShield's
// begin(deviceType, firmwareVersion) declares no module type and
// the Control Server derives it from the one declared device -
// "camera" for a camera, "mono-device" for anything else.
//
// Milestone 39: "esp32-generic" is therefore no longer produced by
// derivation or by the unknown-type fallback. A module carries it
// only when a sketch declared it, which in this tree means a
// Sensor Hub - and that is what lets the app offer Add Node from
// the module type alone.
// ============================================================

namespace ModuleTypes
{
    // A relay controller board. The first genuinely multi-device
    // module type, and the one milestone 36 exists for.
    constexpr const char* RelayBoard    = "relay-board";

    // A HomeShield controller with no more specific type. The
    // honest fallback, not a placeholder.
    //
    // Milestone 38: also what a Sensor Hub registers as. A hub is
    // not given a type of its own - a type decides nothing, and
    // what makes a hub a hub is the node-onboarding CAPABILITY it
    // declares. See ModuleCapabilities.h.
    constexpr const char* Esp32Generic  = "esp32-generic";

    // ------------------------------------------------------------
    // Milestone 39: two new types.
    // ------------------------------------------------------------
    //
    // Neither is a type a sketch normally declares. Both are what
    // the Control Server DERIVES for a board that declares one
    // device and no module type - that is, a board whose sketch
    // calls begin(deviceType, firmwareVersion). They are listed
    // here because this header mirrors ModuleTypeKeys.cs and the
    // two must not drift, not because a sketch is expected to pass
    // them to beginModule().

    // A controller with exactly one addressable function on it -
    // the Light Switch, a door sensor, a siren, a tank sensor.
    //
    // The app draws it as the ordinary Regular Device Card. It
    // exists because the card is now chosen from the module TYPE
    // and never from the number of devices the module holds, so a
    // one-function board needs a type that says so.
    constexpr const char* MonoDevice    = "mono-device";

    // A camera controller running HomeShield firmware - an
    // ESP32-CAM.
    //
    // Derived from the declared device type "camera", so the
    // Camera sketch needs no change. NOT the same thing as
    // "camera-group", which is the logical container the Control
    // Server makes for cameras reached over the network and which
    // no controller may declare.
    constexpr const char* Camera        = "camera";

    // ------------------------------------------------------------
    // Milestone 38: the six sensor-node types are GONE.
    // ------------------------------------------------------------
    //
    //     door-sensor-node   tank-sensor-node   siren-node
    //     pir-node           mmwave-node        ai-vision-node
    //
    // They described a sensor that was its own Module. A sensor
    // node is a DEVICE hosted by a Sensor Hub's Module, so the
    // thing those keys named no longer exists.
    //
    // Nothing in this library or in any sketch read them - every
    // sketch declares a device type and lets the Control Server
    // derive the module type - so removing them is a deletion and
    // not a migration on this side. The Control Server rewrites
    // the rows that still carry the old values.
    //
    // No replacement node types are introduced.
}
