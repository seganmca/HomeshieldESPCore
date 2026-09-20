#pragma once

// Milestone 37: ControlServerUrl is no longer compiled in. The Control Server
// URL is delivered over BLE during onboarding and read from NVS at boot
// (StorageService::LoadProvisioningConfig).
class Configuration
{
public:

    static constexpr auto FirmwareVersion =
        "1.0.0";

    // Milestone 41: HeartbeatInterval is gone. Nothing ever read it -
    // HomeShieldClass owned its own constant - so two numbers claimed to
    // be the heartbeat and only one of them was. The interval is now a
    // settable member of HomeShieldClass, defaulting to 30 s, and this is
    // the only place that fact is not repeated.

    // Milestone 37 (D1). The broker runs on the Control Server's host, so its
    // host is taken from the Control Server URL and only the port is fixed.
    static constexpr auto MqttPort =
        1883;
};
