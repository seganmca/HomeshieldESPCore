#include <StorageService.h>

StorageService storage;

void setup()
{
    Serial.begin(115200);

    delay(1000);

    if (!storage.HasWifiCredentials())
    {
        Serial.println("Saving credentials...");

        storage.SaveWifiCredentials(
            "MyWifi",
            "Password123");
    }

    Serial.print("SSID : ");
    Serial.println(storage.GetWifiSsid());

    Serial.print("Password : ");
    Serial.println(storage.GetWifiPassword());
}

void loop()
{
}