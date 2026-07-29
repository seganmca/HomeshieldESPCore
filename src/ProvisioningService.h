#pragma once

#include "StorageService.h"
#include "RegistrationService.h"
#include <WebServer.h>

class ProvisioningService
{
public:

	ProvisioningService(
		StorageService& storageService,
		HttpService& httpService,
		RegistrationService& registrationService);

    void Begin();

    void Loop();
	
	bool IsConnected() const;

private:

	void ConnectToWifi();
	
    void ConfigureRoutes();

    void HandleRoot();

    void HandleSave();
	
    StorageService& _storageService;
	RegistrationService& _registrationService;

    WebServer _server{80};
	
	HttpService& _httpService;

	unsigned long _lastHeartbeat = 0;

    static constexpr unsigned long HEARTBEAT_INTERVAL = 60000;	
	
};