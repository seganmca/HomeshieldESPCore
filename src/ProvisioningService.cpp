#include "ProvisioningService.h"
#include "Configuration.h"
#include <Arduino.h>
#include <WiFi.h>

ProvisioningService::ProvisioningService(
    StorageService& storageService,
	HttpService& httpService,
    RegistrationService& registrationService)
    : _storageService(storageService),
	  _httpService(httpService),
      _registrationService(registrationService)
{
}

void ProvisioningService::Begin()
{
	if (_storageService.HasWifiCredentials())
	{
		Serial.println("WiFi credentials found.");

		ConnectToWifi();
		
		_registrationService.Register();

		return;
	}

    Serial.println("No WiFi credentials.");

    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    if (WiFi.softAP("HomeShield"))
    {
        Serial.println("Access Point started.");
    }
    else
    {
        Serial.println("Failed to start Access Point.");
    }

    ConfigureRoutes();

    _server.begin();

    Serial.println("Web server started.");

    Serial.print("AP IP : ");
    Serial.println(WiFi.softAPIP());
	
}

void ProvisioningService::Loop()
{
    _server.handleClient();
}

void ProvisioningService::ConfigureRoutes()
{
    _server.on(
        "/",
        [this]()
        {
            HandleRoot();
        });
		
	_server.on(
		"/save",
		HTTP_POST,
		[this]()
		{
			HandleSave();
		});		
}

void ProvisioningService::HandleRoot()
{
    _server.send(
        200,
        "text/html",
        R"(
<!DOCTYPE html>
<html>
<head>
    <title>HomeShield Setup</title>
</head>
<body>
    <h2>HomeShield Setup</h2>

    <form method="POST" action="/save">
        WiFi SSID<br>
		<input type="text" name="ssid"><br><br>

		Password<br>
		<input type="password" name="password"><br><br>

		<input type="submit" value="Save">
    </form>

</body>
</html>
)");
}

void ProvisioningService::HandleSave()
{
    auto ssid =
        _server.arg("ssid");

    auto password =
        _server.arg("password");

    Serial.print("SSID : ");
    Serial.println(ssid);

    Serial.print("Password : ");
    Serial.println(password);

    _storageService.SaveWifiCredentials(
        ssid,
        password);

	_server.send(
		200,
		"text/html",
		"<h2>Configuration Saved.</h2><p>Restarting...</p>");

	delay(1000);

	ESP.restart();
}

void ProvisioningService::ConnectToWifi()
{
    auto ssid =
        _storageService.GetWifiSsid();

    auto password =
        _storageService.GetWifiPassword();

    Serial.print("Connecting to ");
    Serial.println(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
	

    WiFi.begin(
        ssid.c_str(),
        password.c_str());

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);

        Serial.print(".");
    }

    Serial.println();

    Serial.println("WiFi Connected.");

    Serial.print("IP Address : ");
    Serial.println(WiFi.localIP());
}
