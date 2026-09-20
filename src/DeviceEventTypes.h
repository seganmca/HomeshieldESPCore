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


    // ------------------------------------------------------------
    // Milestone 41. A Sensor Hub saying whether one adopted node
    // is still reporting.
    // ------------------------------------------------------------
    //
    // DEVICE-scoped: it carries the node's composed DeviceKey,
    // because it is about one child and not about the board. That
    // is the opposite of NodeDiscovery above, and the difference
    // is load bearing - the Control Server resolves this one to a
    // device and hands it to that device's availability state.
    //
    // Payload: {"online":false} or {"online":true}.
    //
    // NOT a state change. A door sensor whose battery died has not
    // closed the door, and routing this through DeviceStateChanged
    // would write exactly that lie into the history.
    constexpr const char* NodeAvailability   = "NodeAvailability";
}