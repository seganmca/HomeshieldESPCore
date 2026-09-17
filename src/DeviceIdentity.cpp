#include "DeviceIdentity.h"

#include <esp_mac.h>

namespace DeviceIdentity
{
    // Milestone 37 (D10). The factory base MAC from eFuse, read directly so it
    // is available before Wi-Fi is started - an unprovisioned board advertises
    // over BLE with Wi-Fi off.
    //
    // On ESP32, ESP32-C3 and ESP32-S3 the Wi-Fi station MAC IS the base MAC, so
    // this is the same value WiFi.macAddress() returned before M37 and every
    // existing Module.HardwareId still matches. Nothing in HomeShield writes
    // or stores it, so no reset can change it.
    String GetHardwareId()
    {
        static String hardwareId;

        if (hardwareId.length() > 0)
        {
            return hardwareId;
        }

        uint8_t mac[6] = {0};

        esp_read_mac(mac, ESP_MAC_BASE);

        char text[13];

        snprintf(
            text,
            sizeof(text),
            "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        hardwareId = text;

        return hardwareId;
    }
}
