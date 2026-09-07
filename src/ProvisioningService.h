#pragma once

#include "StorageService.h"
#include "RegistrationService.h"
#include <WebServer.h>
#include <DNSServer.h>

class ProvisioningService
{
public:

	ProvisioningService(
		StorageService& storageService,
		HttpService& httpService,
		RegistrationService& registrationService);

    void Begin();

    void Loop();
	
private:

	enum class ProvisioningState
	{
		Idle,
		Connecting,
		Connected,
		Failed
	};

	void ConnectToWifi();

	void StartCaptivePortal();

    void ConfigureRoutes();

    void HandleRoot();

    void HandleSave();

	void HandleScan();

	void RunInitialScan();

	String BuildScanJson(
		int count);

	void HandleStatus();

	void HandleCaptiveRedirect();

	void LogRequest(
		const char* tag);

	void BeginConnectionAttempt();

	void UpdateConnectionAttempt();

	String BuildStatusJson();

	static String Escape(
		const String& value);

    StorageService& _storageService;
	RegistrationService& _registrationService;

    WebServer _server{80};

	DNSServer _dnsServer;

	HttpService& _httpService;

	bool _portalActive = false;

	ProvisioningState _state = ProvisioningState::Idle;

	String _scanJson;

	bool _scanPending = false;

	String _pendingSsid;

	String _pendingPassword;

	unsigned long _lastPortalLog = 0;

	unsigned long _connectStartedAt = 0;

	unsigned long _connectedAt = 0;

	String _failureReason;

	static constexpr unsigned long ConnectTimeout = 25000;

	static constexpr unsigned long RestartDelay = 3000;

	static constexpr byte DnsPort = 53;

	static constexpr const char* ApSsid = "HomeShield";
};
