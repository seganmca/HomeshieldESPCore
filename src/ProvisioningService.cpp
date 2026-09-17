#include "ProvisioningService.h"

#include "Debug.h"
#include "DeviceIdentity.h"
#include "JsonLite.h"

#include <WiFi.h>
#include <NimBLEDevice.h>


namespace
{
    constexpr const char* ServiceUuid =
        "70630001-d6a4-4dd0-99a1-78803d35a375";

    constexpr const char* CommandUuid =
        "70630002-d6a4-4dd0-99a1-78803d35a375";

    constexpr const char* EventUuid =
        "70630003-d6a4-4dd0-99a1-78803d35a375";

    constexpr int ProtocolVersion = 1;


    // The agreed failure categories.
    constexpr const char* WifiConnectionFailed           = "WIFI_CONNECTION_FAILED";
    constexpr const char* ControlServerConnectionFailed  = "CONTROL_SERVER_CONNECTION_FAILED";
    constexpr const char* RegistrationFailed             = "REGISTRATION_FAILED";
    constexpr const char* ConfigurationStorageFailed     = "CONFIGURATION_STORAGE_FAILED";
    constexpr const char* ProvisioningTimeoutCode        = "PROVISIONING_TIMEOUT";
    constexpr const char* InvalidConfiguration           = "INVALID_CONFIGURATION";


    class ServerCallbacks : public NimBLEServerCallbacks
    {
    public:
        explicit ServerCallbacks(ProvisioningService* owner) : _owner(owner) {}

        void onDisconnect(
            NimBLEServer* server,
            NimBLEConnInfo& connInfo,
            int reason) override
        {
            _owner->OnClientDisconnected();
        }

    private:
        ProvisioningService* _owner;
    };


    class CommandCallbacks : public NimBLECharacteristicCallbacks
    {
    public:
        explicit CommandCallbacks(ProvisioningService* owner) : _owner(owner) {}

        void onWrite(
            NimBLECharacteristic* characteristic,
            NimBLEConnInfo& connInfo) override
        {
            const NimBLEAttValue value =
                characteristic->getValue();

            _owner->OnCommandWritten(
                value.data(),
                value.length());
        }

    private:
        ProvisioningService* _owner;
    };


    class EventCallbacks : public NimBLECharacteristicCallbacks
    {
    public:
        explicit EventCallbacks(ProvisioningService* owner) : _owner(owner) {}

        // Called when an indication is confirmed, fails or times out.
        void onStatus(
            NimBLECharacteristic* characteristic,
            NimBLEConnInfo& connInfo,
            int code) override
        {
            _owner->OnIndicationFinished();
        }

    private:
        ProvisioningService* _owner;
    };
}


ProvisioningService::ProvisioningService(
    StorageService& storageService,
    RegistrationService& registrationService)
    : _storageService(storageService),
      _registrationService(registrationService)
{
}


bool ProvisioningService::IsProvisioned() const
{
    return _state == State::Provisioned;
}


const String& ProvisioningService::GetControlServerUrl() const
{
    return _config.controlServerUrl;
}


void ProvisioningService::Begin()
{
    // The provisioning configuration lives in HomeShield's own NVS keys. The
    // Wi-Fi driver must not keep a second copy that a reset would not clear.
    WiFi.persistent(false);

    if (_storageService.LoadProvisioningConfig(_config))
    {
        DEBUG_LOG("Provisioned. Connecting to the stored Wi-Fi.");

        _state = State::Provisioned;

        _registrationService.SetControlServerUrl(
            _config.controlServerUrl);

        WiFi.mode(WIFI_STA);

        WiFi.setTxPower(WIFI_POWER_8_5dBm);

        WiFi.begin(
            _config.wifiSsid.c_str(),
            _config.wifiPassword.c_str());

        return;
    }

    // A board that carries only the pre-M37 Wi-Fi keys lands here too: it has
    // no Control Server URL and no commit marker, so it is onboarded over BLE.
    Serial.println(
        "[HomeShield] Not provisioned. Advertising for BLE onboarding as HS-" +
        DeviceIdentity::GetHardwareId());

    _config = ProvisioningConfig();

    _state = State::Unprovisioned;

    StartBle();
}


void ProvisioningService::StartBle()
{
    String name =
        "HS-" + DeviceIdentity::GetHardwareId();

    NimBLEDevice::init(name.c_str());

    NimBLEDevice::setMTU(247);

    // LE Secure Connections, Just Works, no bonding (D6).
    NimBLEDevice::setSecurityAuth(false, false, true);

    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    NimBLEServer* server =
        NimBLEDevice::createServer();

    server->setCallbacks(new ServerCallbacks(this));

    // Advertising is resumed by Loop(), and only while unprovisioned.
    server->advertiseOnDisconnect(false);

    NimBLEService* service =
        server->createService(ServiceUuid);

    // Encrypted write: the first command triggers pairing, so the Wi-Fi
    // password never crosses an unencrypted link.
    NimBLECharacteristic* command =
        service->createCharacteristic(
            CommandUuid,
            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);

    command->setCallbacks(new CommandCallbacks(this));

    _eventCharacteristic =
        service->createCharacteristic(
            EventUuid,
            NIMBLE_PROPERTY::INDICATE);

    _eventCharacteristic->setCallbacks(new EventCallbacks(this));

    service->start();

    StartAdvertising();
}


void ProvisioningService::StartAdvertising()
{
    NimBLEAdvertising* advertising =
        NimBLEDevice::getAdvertising();

    // The name goes in the advertisement, the 128-bit service UUID in the
    // scan response: both do not fit in 31 bytes.
    NimBLEAdvertisementData advertisement;

    advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);

    advertisement.setName(
        std::string("HS-") + DeviceIdentity::GetHardwareId().c_str());

    NimBLEAdvertisementData scanResponse;

    scanResponse.addServiceUUID(NimBLEUUID(ServiceUuid));

    advertising->setAdvertisementData(advertisement);

    advertising->setScanResponseData(scanResponse);

    advertising->start();
}


bool ProvisioningService::HasClient() const
{
    NimBLEServer* server =
        NimBLEDevice::getServer();

    return server != nullptr &&
        server->getConnectedCount() > 0;
}


// ==================================================
// BLE host task -> Loop()
// ==================================================

void ProvisioningService::OnCommandWritten(
    const uint8_t* data,
    size_t length)
{
    if (length > MaxMessageLength)
    {
        length = MaxMessageLength;
    }

    portENTER_CRITICAL(&_inboxLock);

    memcpy(_inbox, data, length);

    _inbox[length] = '\0';

    _hasInbox = true;

    portEXIT_CRITICAL(&_inboxLock);
}


void ProvisioningService::OnClientDisconnected()
{
    _clientDisconnected = true;
}


void ProvisioningService::OnIndicationFinished()
{
    _indicationFinished = true;
}


// ==================================================
// Loop
// ==================================================

void ProvisioningService::Loop()
{
    if (_state == State::Provisioned)
    {
        return;
    }


    if (_hasInbox)
    {
        char message[MaxMessageLength + 1];

        portENTER_CRITICAL(&_inboxLock);

        memcpy(message, _inbox, sizeof(message));

        _hasInbox = false;

        portEXIT_CRITICAL(&_inboxLock);

        HandleCommand(String(message));
    }


    if (_clientDisconnected)
    {
        _clientDisconnected = false;

        // Before commit, a phone that leaves takes its configuration with it.
        // After commit the transaction carries on without the phone.
        if (_state == State::Unprovisioned)
        {
            DiscardPending();

            StartAdvertising();
        }
    }


    if (_state == State::Provisioning)
    {
        UpdateProvisioning();
    }
}


// ==================================================
// Commands
// ==================================================

void ProvisioningService::HandleCommand(
    const String& json)
{
    String op;

    long id = 0;

    JsonLite::ReadLong(json, "id", id);

    if (!JsonLite::ReadString(json, "op", op))
    {
        SendAck(id, false);

        return;
    }


    if (op == "GET_IDENTITY")
    {
        Send(
            "{\"type\":\"IDENTITY_RESPONSE\",\"id\":" + String(id) +
            ",\"hardwareId\":\"" + DeviceIdentity::GetHardwareId() +
            "\",\"protocolVersion\":" + String(ProtocolVersion) + "}");

        return;
    }


    // Configuration is only accepted while nothing is in progress.
    if (_state != State::Unprovisioned)
    {
        SendAck(id, false);

        return;
    }


    if (op == "SET_WIFI_CONFIGURATION")
    {
        String ssid;
        String password;

        bool valid =
            JsonLite::ReadString(json, "ssid", ssid) &&
            JsonLite::ReadString(json, "password", password) &&
            IsValidSsid(ssid) &&
            IsValidPassword(password);

        if (valid)
        {
            _pending.wifiSsid = ssid;
            _pending.wifiPassword = password;
            _hasWifi = true;
        }

        SendAck(id, valid);

        return;
    }


    if (op == "SET_CONTROL_SERVER_CONFIGURATION")
    {
        String url;

        bool valid =
            JsonLite::ReadString(json, "controlServerUrl", url);

        while (valid && url.endsWith("/"))
        {
            url.remove(url.length() - 1);
        }

        valid = valid && IsValidUrl(url);

        if (valid)
        {
            _pending.controlServerUrl = url;
            _hasControlServer = true;
        }

        SendAck(id, valid);

        return;
    }


    if (op == "COMMIT_PROVISIONING")
    {
        bool ready =
            _hasWifi &&
            _hasControlServer;

        SendAck(id, ready);

        if (ready)
        {
            _commitId = id;

            Commit();
        }

        return;
    }


    SendAck(id, false);
}


void ProvisioningService::Send(
    const String& json)
{
    if (_eventCharacteristic == nullptr || !HasClient())
    {
        return;
    }

    DEBUG_VALUE("[BLE] ->", json);

    _eventCharacteristic->indicate(
        (const uint8_t*)json.c_str(),
        json.length());
}


void ProvisioningService::SendAck(
    long id,
    bool ok)
{
    if (ok)
    {
        Send("{\"type\":\"ACK\",\"id\":" + String(id) + ",\"ok\":true}");
    }
    else
    {
        Send(
            "{\"type\":\"ACK\",\"id\":" + String(id) +
            ",\"ok\":false,\"code\":\"" + InvalidConfiguration + "\"}");
    }
}


// ==================================================
// The transaction
// ==================================================

void ProvisioningService::Commit()
{
    DEBUG_VALUE("Provisioning: joining ", _pending.wifiSsid);

    _state = State::Provisioning;

    _commitAt = millis();

    _wifiStartedAt = millis();

    _wifiJoined = false;

    _restarting = false;

    WiFi.mode(WIFI_STA);

    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    WiFi.begin(
        _pending.wifiSsid.c_str(),
        _pending.wifiPassword.c_str());
}


void ProvisioningService::UpdateProvisioning()
{
    if (_restarting)
    {
        if (_indicationFinished ||
            millis() - _succeededAt >= RestartDelay)
        {
            DEBUG_LOG("Provisioned. Restarting.");

            delay(100);

            ESP.restart();
        }

        return;
    }


    if (millis() - _commitAt >= ProvisioningTimeout)
    {
        Fail(ProvisioningTimeoutCode);

        return;
    }


    // 1. Wi-Fi.
    if (!_wifiJoined)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            _wifiJoined = true;

            DEBUG_VALUE("Provisioning: Wi-Fi joined, IP ", WiFi.localIP());

            // 2. The existing registration, against the supplied URL.
            _registrationService.SetControlServerUrl(
                _pending.controlServerUrl);
        }
        else if (millis() - _wifiStartedAt >= WifiConnectTimeout)
        {
            Fail(WifiConnectionFailed);
        }

        return;
    }


    if (_registrationService.IsRegistered())
    {
        Succeed();

        return;
    }


    int status =
        _registrationService.LastStatusCode();

    if (status == 422 ||
        _registrationService.FailedAttempts() >= MaxRegistrationFailures)
    {
        // No HTTP response at all means the server was never reached.
        Fail(
            status > 0
                ? RegistrationFailed
                : ControlServerConnectionFailed);
    }
}


void ProvisioningService::Succeed()
{
    // 3. Persist, only now.
    if (!_storageService.SaveProvisioningConfig(_pending))
    {
        Fail(ConfigurationStorageFailed);

        return;
    }

    // 4. Report.
    _indicationFinished = false;

    Send(
        "{\"type\":\"PROVISIONING_SUCCESS\",\"id\":" + String(_commitId) +
        ",\"moduleId\":" + String(_registrationService.ModuleId()) + "}");

    DiscardPending();

    // 5. Restart once the phone has confirmed the indication, or shortly
    // after. The next boot runs from NVS.
    _restarting = true;

    _succeededAt = millis();
}


void ProvisioningService::Fail(
    const char* code)
{
    Serial.print("[HomeShield] Provisioning failed: ");
    Serial.println(code);

    // Nothing was persisted. Stop registering and leave the network.
    _registrationService.SetControlServerUrl("");

    WiFi.disconnect(true);

    DiscardPending();

    Send(
        "{\"type\":\"PROVISIONING_FAILED\",\"id\":" + String(_commitId) +
        ",\"code\":\"" + code + "\"}");

    _state = State::Unprovisioned;

    if (!HasClient())
    {
        StartAdvertising();
    }
}


void ProvisioningService::DiscardPending()
{
    _pending = ProvisioningConfig();

    _hasWifi = false;

    _hasControlServer = false;
}


// ==================================================
// Validation
// ==================================================

bool ProvisioningService::IsValidSsid(
    const String& ssid)
{
    return ssid.length() >= 1 &&
        ssid.length() <= 32;
}


bool ProvisioningService::IsValidPassword(
    const String& password)
{
    size_t length =
        password.length();

    if (length == 0) return true;

    if (length >= 8 && length <= 63) return true;

    if (length != 64) return false;

    for (size_t i = 0; i < length; i++)
    {
        if (!isHexadecimalDigit(password[i])) return false;
    }

    return true;
}


bool ProvisioningService::IsValidUrl(
    const String& url)
{
    if (url.length() > 128 ||
        !url.startsWith("http://"))
    {
        return false;
    }

    String rest =
        url.substring(7);

    if (rest.indexOf('/') >= 0 ||
        rest.indexOf('?') >= 0)
    {
        return false;
    }

    int colon =
        rest.indexOf(':');

    String host =
        colon < 0 ? rest : rest.substring(0, colon);

    if (host.length() == 0)
    {
        return false;
    }

    if (colon >= 0)
    {
        String port =
            rest.substring(colon + 1);

        if (port.length() == 0 || port.length() > 5) return false;

        for (size_t i = 0; i < port.length(); i++)
        {
            if (!isDigit(port[i])) return false;
        }

        long value = port.toInt();

        if (value < 1 || value > 65535) return false;
    }

    return true;
}


String ProvisioningService::HostFromUrl(
    const String& url)
{
    String rest =
        url.startsWith("http://") ? url.substring(7) : url;

    int end =
        rest.length();

    int colon = rest.indexOf(':');
    int slash = rest.indexOf('/');

    if (colon >= 0 && colon < end) end = colon;
    if (slash >= 0 && slash < end) end = slash;

    return rest.substring(0, end);
}
