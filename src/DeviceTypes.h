#pragma once

namespace DeviceTypes
{
    constexpr const char* DoorSensor   = "door-sensor";

    constexpr const char* TankSensor = "float-sensor";

    constexpr const char* MotorNode    = "motor-actuator";

    constexpr const char* Camera       = "camera";

    constexpr const char* AiVision       = "ai-vision";

    constexpr const char* Siren        = "siren";

    constexpr const char* Light        = "switch";

    constexpr const char* PIR          = "pir-sensor";

    constexpr const char* mmWaveRadar  = "mmwave-radar";

    // M47. Full-duplex audio intercom (XIAO ESP32-S3 Sense + MAX98357A),
    // a mono-device. Mirrors DeviceTypeKeys.AudioIntercom on the servers.
    constexpr const char* AudioIntercom = "audio-intercom";

}
