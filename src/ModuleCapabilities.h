#pragma once

// ============================================================
// Module Capabilities
// ============================================================
//
// Milestone 38. What a module can DO, as opposed to what kind of
// thing it is.
//
// Mirrors HomeShield.Common/Constants/ModuleCapabilityKeys.cs the
// same way ModuleTypes.h mirrors ModuleTypeKeys.cs. Two
// declarations of the same string drift in silence, so if a key
// changes there it must change here.
//
// This exists because module TYPE could not answer the question
// M38 asks. A type is presentational (M36 decision D2) and
// decides nothing, and M38 was explicitly not allowed to invent
// a "hub" type. But the app has to know which modules may offer
// Add Node, and "every esp32-generic board" is the wrong answer:
// a brand new generic board and an empty Sensor Hub would be
// indistinguishable.
//
// So a module DECLARES what it can do, the Control Server records
// it, and the app reads it. A capability is behavioural where a
// type is cosmetic, and that is the whole distinction.
//
// Declaring is optional and additive. A board that declares none
// - which is every board flashed before this milestone - is
// unaffected, and no existing firmware needs reflashing.
// ============================================================

namespace ModuleCapabilities
{
    // ------------------------------------------------------------
    // This module can adopt sensor nodes over ESP-NOW.
    // ------------------------------------------------------------
    //
    // Declared by HomeShield.ESP.SensorHub and by nothing else.
    // It is what makes Add Node appear on that module's details
    // screen and on no other - a Relay Board does not declare it,
    // and neither does an ordinary esp32-generic board.
    //
    // The Control Server re-checks it before it starts a
    // discovery, rather than trusting the app to have hidden the
    // button. A client-side check is a convenience; this is the
    // rule.
    constexpr const char* NodeOnboarding = "node-onboarding";
}
