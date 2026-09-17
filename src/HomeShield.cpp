#include "Debug.h"
#include "HomeShield.h"
#include "DeviceEventTypes.h"
#include "Configuration.h"
#include "JsonLite.h"

#include <WiFi.h>


HomeShieldClass HomeShield;


HomeShieldClass*
HomeShieldClass::_instance =
    nullptr;


// ==================================================
// Declaring devices
// ==================================================

void HomeShieldClass::addDevice(
    const String& deviceKey,
    const String& deviceType,
    const String& defaultName)
{
    // The declaration has to be complete before registration
    // starts, because the registration task reads the array as it
    // stands the moment it is created.
    if (_started)
    {
        Serial.println(
            "[HomeShield] addDevice() was called after the module had already "
            "started. It is ignored. Declare every device BEFORE calling "
            "beginModule().");

        return;
    }


    if (_deviceCount >= MAX_DEVICES)
    {
        Serial.print(
            "[HomeShield] addDevice() refused: this library declares at most ");

        Serial.print(MAX_DEVICES);

        Serial.print(
            " devices per module, and '");

        Serial.print(deviceKey);

        Serial.println(
            "' would exceed it.");

        return;
    }


    if (deviceKey.length() == 0 ||
        deviceType.length() == 0)
    {
        Serial.println(
            "[HomeShield] addDevice() refused: a device needs both a device key "
            "and a device type.");

        return;
    }


    // Caught here as well as by the Control Server. The server
    // refuses the whole declaration with a 422, and the
    // registration task would then retry it every ten seconds
    // forever - a local check turns that into one message at boot.
    if (IndexOf(deviceKey) >= 0)
    {
        Serial.print(
            "[HomeShield] addDevice() refused: device key '");

        Serial.print(deviceKey);

        Serial.println(
            "' is already declared. Device keys must be unique within a module.");

        return;
    }


    _devices[_deviceCount].deviceKey = deviceKey;
    _devices[_deviceCount].deviceType = deviceType;
    _devices[_deviceCount].defaultName = defaultName;

    _deviceCount++;
}


int HomeShieldClass::IndexOf(
    const String& deviceKey) const
{
    for (int i = 0; i < _deviceCount; i++)
    {
        if (_devices[i].deviceKey == deviceKey) return i;
    }

    return -1;
}


// ==================================================
// Starting up
// ==================================================

void HomeShieldClass::begin(
    const String& deviceType,
    const String& firmwareVersion)
{
    // A sketch that declared devices and then called begin() meant
    // beginModule(). Refused rather than guessed: continuing would
    // register the board under an identity its author did not
    // choose, and a wrong identity in the database is far worse to
    // unpick than a board that visibly did not come up.
    if (_deviceCount > 0)
    {
        Serial.println(
            "[HomeShield] begin() was called after addDevice(). A module that "
            "declares its own devices must start with beginModule(moduleType, "
            "firmwareVersion). Nothing has been started.");

        return;
    }


    // The library declares the key, not the sketch. See
    // DeviceKeys::Main for why this one line is the reason a
    // reflashed single-device board keeps its Device.Id.
    addDevice(
        DeviceKeys::Main,
        deviceType);


    if (_deviceCount != 1)
    {
        Serial.println(
            "[HomeShield] begin() could not declare this device. Nothing has "
            "been started.");

        return;
    }


    // No module type. With exactly one declared device the Control
    // Server derives it, which is how a reflashed board keeps the
    // module type it already had.
    Start(
        String(""),
        firmwareVersion);
}


void HomeShieldClass::beginModule(
    const String& moduleType,
    const String& firmwareVersion)
{
    if (_deviceCount == 0)
    {
        Serial.println(
            "[HomeShield] beginModule() was called with no devices declared. "
            "Call addDevice(deviceKey, deviceType) for each device first. "
            "Nothing has been started.");

        return;
    }


    if (moduleType.length() == 0)
    {
        Serial.println(
            "[HomeShield] beginModule() was called with no module type. Pass one "
            "from ModuleTypes.h. Nothing has been started.");

        return;
    }


    Start(
        moduleType,
        firmwareVersion);
}


void HomeShieldClass::Start(
    const String& moduleType,
    const String& firmwareVersion)
{
    _instance = this;


    _firmwareVersion =
        firmwareVersion;


    _moduleType =
        moduleType;


    _lastHeartbeat =
        0;


    _mqttInitialized =
        false;


    _wasWifiConnected =
        false;


    // Marked started before the services are created: addDevice()
    // must be refused from here on, and the registration task reads
    // the declared array as soon as it is running.
    _started =
        true;


    _registrationService =
        new RegistrationService(
            _storageService,
            _httpService,
            _moduleType,
            _firmwareVersion,
            _devices,
            _deviceCount);


    if (_resetPin >= 0)
    {
        pinMode(
            _resetPin,
            _resetActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);
    }
    else
    {
        Serial.println(
            "[HomeShield] No provisioning-reset button configured. Call "
            "HomeShield.setProvisioningResetButton(pin) before begin() to "
            "allow this board to be re-onboarded.");
    }


    // Milestone 37. Provisioned: joins the stored Wi-Fi and gives the stored
    // Control Server URL to registration. Otherwise: BLE onboarding.
    _provisioningService =
        new ProvisioningService(
            _storageService,
            *_registrationService);


    _provisioningService->Begin();


    _mqttHost =
        ProvisioningService::HostFromUrl(
            _provisioningService->GetControlServerUrl());


    // Start the registration task.
    // The task itself waits for WiFi and a Control Server URL, and retries
    // until registration succeeds.
    _registrationService->Begin();
}


void HomeShieldClass::loop()
{
    /*
     * IMPORTANT:
     *
     * This function must remain fast.
     *
     * The physical device application calls this
     * function from its main loop, so no blocking
     * network operation is performed here.
     */


    // Nothing was started, because the declaration was refused. The
    // message explaining why has already been printed once; this
    // just keeps the board from half-working.
    if (!_started)
    {
        return;
    }


    CheckResetButton();


    if (_provisioningService != nullptr)
    {
        _provisioningService->Loop();
    }


    // Milestone 37. Until the board is provisioned there is no broker to talk
    // to - MQTT is not part of onboarding - so everything below waits for the
    // restart that follows a successful provisioning.
    if (_provisioningService == nullptr ||
        !_provisioningService->IsProvisioned())
    {
        return;
    }


    bool wifiConnected =
        IsConnected();


    /*
     * Detect WiFi loss.
     */
    if (!wifiConnected)
    {
        if (_wasWifiConnected)
        {
            DEBUG_LOG(
                "WiFi disconnected.");

            _mqttService.disconnect();

            _mqttInitialized =
                false;

            _wasWifiConnected =
                false;
        }

        return;
    }


    /*
     * Detect WiFi reconnection.
     */
    if (!_wasWifiConnected)
    {
        DEBUG_LOG(
            "WiFi connected.");

        _wasWifiConnected =
            true;

        _mqttInitialized =
            false;

        _lastHeartbeat =
            millis();
    }


    /*
     * Initialize MQTT once per WiFi session.
     */
    if (!_mqttInitialized)
    {
        _mqttService.begin(
            _mqttHost.c_str(),
            Configuration::MqttPort);


        _mqttService.setCallback(
            OnMqttMessage);


        // ONE subscription for the whole module, whatever its
        // channel count. The topic addresses the CONTROLLER - which
        // is what it always did - and the DeviceKey inside the
        // payload says which device on it a command is for. Per
        // device topics would need one subscription each and
        // MqttService allows ten in total, which a ten-relay board
        // would exhaust exactly.
        String topic =
            "homeshield/device/" +
            DeviceIdentity::GetHardwareId();


        _mqttService.addSubscription(
            topic);


        _mqttInitialized =
            true;


        _lastHeartbeat =
            millis();


        DEBUG_LOG(
            "MQTT service initialized.");
    }


    /*
     * MQTT processing is non-blocking.
     *
     * Failed connections result in a single
     * connection attempt every few seconds.
     */
    _mqttService.loop();


    /*
     * Heartbeat.
     */
    if (MqttConnected())
    {
        if (millis() -
            _lastHeartbeat >=
            HEARTBEAT_INTERVAL)
        {
            publishHeartbeat();

            _lastHeartbeat =
                millis();
        }
    }
}


void HomeShieldClass::setProvisioningResetButton(
    int pin,
    bool activeLow)
{
    if (_started)
    {
        Serial.println(
            "[HomeShield] setProvisioningResetButton() must be called before "
            "begin()/beginModule(). It is ignored.");

        return;
    }

    _resetPin = pin;

    _resetActiveLow = activeLow;
}


void HomeShieldClass::CheckResetButton()
{
    if (_resetPin < 0)
    {
        return;
    }

    bool pressed =
        digitalRead(_resetPin) == (_resetActiveLow ? LOW : HIGH);

    if (!pressed)
    {
        _resetPressed = false;

        return;
    }

    if (!_resetPressed)
    {
        _resetPressed = true;

        _resetPressedAt = millis();

        return;
    }

    // Held continuously for 5 s. Any release in between restarts the count,
    // which is also what filters contact bounce.
    if (millis() - _resetPressedAt < RESET_HOLD_TIME)
    {
        return;
    }

    Serial.println(
        "[HomeShield] Provisioning reset. Clearing configuration and restarting.");

    _storageService.ClearProvisioningConfig();

    WiFi.disconnect(true, true);

    delay(100);

    ESP.restart();
}


void HomeShieldClass::setCommandHandler(
    CommandHandler handler)
{
    _commandHandler =
        handler;
}


// ==================================================
// Inbound commands
// ==================================================

void HomeShieldClass::OnMqttMessage(
    char* topic,
    byte* payload,
    unsigned int length)
{
    if (_instance == nullptr)
        return;


    String message;

    message.reserve(
        length);


    for (unsigned int i = 0;
         i < length;
         i++)
    {
        message +=
            (char)payload[i];
    }


    DEBUG_VALUE(
        "Command Received: ",
        message);


    _instance->Dispatch(
        message);
}


void HomeShieldClass::Dispatch(
    const String& payload)
{
    if (_commandHandler == nullptr)
        return;


    String body = payload;

    body.trim();


    if (body.length() == 0)
        return;


    String deviceKey;
    String command;


    if (body[0] == '{')
    {
        // v2 envelope: {"deviceKey":"relay1","command":"ON"}
        if (!JsonLite::ReadString(body, "command", command))
        {
            DEBUG_LOG(
                "Dropped a command: the envelope carries no 'command'.");

            return;
        }

        // A missing deviceKey in an envelope is treated the same as
        // a bare command, which is the honest reading: the message
        // did not say which device it meant.
        JsonLite::ReadString(body, "deviceKey", deviceKey);
    }
    else
    {
        // v1 bare command - "ON", "OFF", "ON:30", "CAPTURE:5". This
        // is what the Control Server still sends to a module holding
        // one device, so an unreflashed board and a reflashed one are
        // fed the same thing.
        command = body;
    }


    int index = -1;


    if (deviceKey.length() > 0)
    {
        index = IndexOf(deviceKey);

        if (index < 0)
        {
            // Dropped, not guessed. The board was told to act on a
            // device it does not have.
            DEBUG_VALUE(
                "Dropped a command for an unknown device key: ",
                deviceKey);

            return;
        }
    }
    else
    {
        // No key. It belongs to the module's single device if there
        // is exactly one, and is genuinely ambiguous otherwise -
        // mirroring IRuntimeModule.SingleDevice on the server side.
        if (_deviceCount == 1)
        {
            index = 0;
        }
        else
        {
            DEBUG_LOG(
                "Dropped a command: it carries no device key and this module "
                "does not have exactly one device to fall back to.");

            return;
        }
    }


    // The RESOLVED declared key, never an empty string - so a
    // single-device sketch is handed DeviceKeys::Main and the
    // argument always means something.
    _commandHandler(
        _devices[index].deviceKey,
        command);
}


// ==================================================
// Outbound state and events
// ==================================================

bool HomeShieldClass::publishEvent(
    const String& deviceKey,
    const String& eventType,
    const String& payloadJson)
{
    String request =
        "{"
        "\"hardwareId\":\"" +
        DeviceIdentity::GetHardwareId() +
        "\",";


    // Omitted when empty, and that is the single-device case. The
    // Control Server resolves a keyless message to the module's only
    // device, so the envelope stays exactly what these boards have
    // always sent.
    if (deviceKey.length() > 0)
    {
        request +=
            "\"deviceKey\":\"" +
            deviceKey +
            "\",";
    }


    request +=
        "\"eventType\":\"" +
        eventType +
        "\","
        "\"payload\":" +
        payloadJson +
        "}";


    return publish(
        EVENTS_TOPIC,
        request);
}


bool HomeShieldClass::publishEvent(
    const String& eventType,
    const String& payloadJson)
{
    if (_deviceCount != 1)
    {
        Serial.println(
            "[HomeShield] A key-less publish was refused: this module holds more "
            "than one device, so the report must name one with "
            "publishState(deviceKey, state) or publishEvent(deviceKey, ...).");

        return false;
    }


    return publishEvent(
        String(""),
        eventType,
        payloadJson);
}


bool HomeShieldClass::publishState(
    const String& deviceKey,
    int state)
{
    return publishEvent(
        deviceKey,
        DeviceEventTypes::DeviceStateChanged,
        "{\"state\":" + String(state) + "}");
}


bool HomeShieldClass::publishState(
    int state)
{
    return publishEvent(
        DeviceEventTypes::DeviceStateChanged,
        "{\"state\":" + String(state) + "}");
}


bool HomeShieldClass::publish(
    const String& topic,
    const String& message)
{
    if (!MqttConnected())
    {
        return false;
    }


    DEBUG_VALUE(
        "Publishing event :",
        message);


    return _mqttService.publish(
        topic,
        message);
}


String HomeShieldClass::GetHardwareId()
{
    return DeviceIdentity::GetHardwareId();
}


bool HomeShieldClass::IsConnected() const
{
    return WiFi.status() ==
        WL_CONNECTED;
}


void HomeShieldClass::publishHeartbeat()
{
    // MODULE-SCOPED, and unchanged on the wire. A heartbeat is about
    // the BOARD: one firmware version, one radio, one battery,
    // however many devices hang off it. It carries no DeviceKey and
    // never did, which is exactly what milestone 36 needs - the
    // Control Server records it against the Module.
    int wifiStrength =
        WiFi.RSSI();


    String request =
        "{"
        "\"hardwareId\":\"" +
        DeviceIdentity::GetHardwareId() +
        "\","
        "\"eventType\":\"Heartbeat\","
        "\"payload\":{"
        "\"firmwareVersion\":\"" +
        _firmwareVersion +
        "\","
        "\"wifiStrength\":" +
        String(wifiStrength);


    if (_telemetry.batteryLevel >= 0)
    {
        request +=
            ",\"batteryLevel\":" +
            String(_telemetry.batteryLevel);
    }


    request +=
        "}"
        "}";


    publish(
        EVENTS_TOPIC,
        request);
}


bool HomeShieldClass::MqttConnected()
{
    return
        _mqttInitialized &&
        _mqttService.connected();
}

void HomeShieldClass::SetBatteryLevel(
    int batteryLevel)
{
    if (batteryLevel < 0)
    {
        _telemetry.batteryLevel = -1;
        return;
    }

    if (batteryLevel > 100)
    {
        batteryLevel = 100;
    }

    _telemetry.batteryLevel =
        batteryLevel;
}
