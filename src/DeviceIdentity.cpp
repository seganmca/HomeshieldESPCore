#include "DeviceIdentity.h"

#include <WiFi.h>

namespace DeviceIdentity
{
    String GetHardwareId()
    {
        String hardwareId = WiFi.macAddress();

        hardwareId.replace(":", "");
        hardwareId.toUpperCase();

        return hardwareId;
    }
}