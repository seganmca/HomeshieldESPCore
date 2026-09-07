#pragma once

#include "HttpService.h"
#include "StorageService.h"

class RegistrationService
{
public:

	RegistrationService(
		StorageService& storageService,
		HttpService& httpService,
		const String& deviceType);

    void Register();

private:

    StorageService& _storageService;

    HttpService& _httpService;

    String _deviceType;
};