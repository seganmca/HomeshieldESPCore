#pragma once

namespace DeviceEventTypes
{
    constexpr const char* DeviceStateChanged = "DeviceStateChanged";
    constexpr const char* Heartbeat          = "Heartbeat";
    constexpr const char* MotionDetected     = "MotionDetected";
    constexpr const char* ImageCaptured      = "ImageCaptured";

    // ------------------------------------------------------------
    // Milestone 38. A Sensor Hub reporting how an onboarding is
    // going.
    // ------------------------------------------------------------
    //
    // MODULE-scoped: it carries no DeviceKey, because there is no
    // child device yet - creating one is what the sequence is
    // trying to do. The Control Server routes it to the node
    // onboarding service and nowhere else; it is not a state
    // change and no DeviceEvent row is written for it.
    constexpr const char* NodeDiscovery      = "NodeDiscovery";
}