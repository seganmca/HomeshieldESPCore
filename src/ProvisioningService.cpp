#include "ProvisioningService.h"
#include "Configuration.h"
#include <Arduino.h>
#include <WiFi.h>
#include "Debug.h"

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
		DEBUG_LOG("WiFi credentials found.");

		ConnectToWifi();
		
		_registrationService.Register();

		return;
	}

    DEBUG_LOG("No WiFi credentials.");

    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    if (WiFi.softAP("HomeShield"))
    {
        DEBUG_LOG("Access Point started.");
    }
    else
    {
        DEBUG_LOG("Failed to start Access Point.");
    }

    ConfigureRoutes();

    _server.begin();

    DEBUG_LOG("Web server started.");

    DEBUG_VALUE("AP IP : ", WiFi.softAPIP());
	
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

    DEBUG_VALUE("SSID : ", ssid);

    DEBUG_VALUE("Password : ", password);

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

    DEBUG_VALUE("Connecting to ", ssid);

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
	

    WiFi.begin(
        ssid.c_str(),
        password.c_str());

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);

        DEBUG_LOG_PRINT(".");
    }

    DEBUG_LOG("WiFi Connected.");

    DEBUG_VALUE("IP Address : ", WiFi.localIP());
}
