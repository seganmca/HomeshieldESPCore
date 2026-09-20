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
        DEBUG_LOG(
            "[HomeShield] addDevice() was called after the module had already "
            "started. It is ignored. Declare every device BEFORE calling "
            "beginModule().");

        return;
    }


    if (_deviceCount >= MAX_DEVICES)
    {
        DEBUG_LOG_PRINT(
            "[HomeShield] addDevice() refused: this library declares at most ");

        DEBUG_LOG_PRINT(MAX_DEVICES);

        DEBUG_LOG_PRINT(
            " devices per module, and '");

        DEBUG_LOG_PRINT(deviceKey);

        DEBUG_LOG(
            "' would exceed it.");

        return;
    }


    if (deviceKey.length() == 0 ||
        deviceType.length() == 0)
    {
        DEBUG_LOG(
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
        DEBUG_LOG_PRINT(
            "[HomeShield] addDevice() refused: device key '");

        DEBUG_LOG_PRINT(deviceKey);

        DEBUG_LOG(
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
        DEBUG_LOG(
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
        DEBUG_LOG(
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
    // Milestone 38 (decision A38-4). ZERO devices is now legitimate, and this
    // refusal is gone rather than relaxed.
    //
    // A Sensor Hub registers before it has adopted anything, and it must: the
    // Control Server cannot be told to start a discovery on a module it has
    // never heard of. It declares no child because it HAS no child - there is
    // deliberately no representative or placeholder device standing in for the
    // nodes it does not have yet.
    //
    // The module type below is what makes that safe. With no devices there is
    // nothing to derive a type from, so a type must be declared, and the check
    // that follows is now load-bearing for two cases rather than one.
    if (_deviceCount == 0)
    {
        DEBUG_LOG(
            "[HomeShield] beginModule() was called with no devices declared. "
            "That is expected for a Sensor Hub before any node has been "
            "onboarded; it is a mistake on any other board.");
    }


    if (moduleType.length() == 0)
    {
        DEBUG_LOG(
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
            _deviceCount,
            _capabilityCount > 0 ? _capabilities : nullptr,
            _capabilityCount);


    if (_resetPin >= 0)
    {
        pinMode(
            _resetPin,
            _resetActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);
    }
    else
    {
        DEBUG_LOG(
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


        // Milestone 40. The unprovisioning topic, subscribed alongside the
        // command topic and at QoS 1 - the command is published once and
        // nothing retries it. addSubscription() is idempotent, so the
        // re-initialisation that follows every Wi-Fi reconnect re-subscribes
        // both without consuming another slot.
        _mqttService.addSubscription(
            DeviceIdentity::GetHardwareId() +
                PROVISIONING_COMMAND_SUFFIX,
            1);


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
     * Unprovisioning (milestone 40).
     *
     * Checked here, first among the drains and before the heartbeat, because
     * it does not return: the board clears its configuration and restarts.
     * Anything queued behind it belongs to a module that has just been
     * deleted and has nowhere to go.
     */
    if (_unprovisionRequested)
    {
        _unprovisionRequested = false;

        Unprovision();

        return;
    }


    /*
     * Module commands.
     *
     * Drained HERE, immediately after the MQTT client has finished its own
     * loop, so the handler runs with PubSubClient idle and may publish freely.
     * Dispatch() only queues; see _moduleCommandInbox for why.
     */
    if (_hasModuleCommand)
    {
        char command[MAX_MODULE_COMMAND + 1];

        portENTER_CRITICAL(&_moduleCommandLock);

        memcpy(command, _moduleCommandInbox, sizeof(command));

        _hasModuleCommand = false;

        portEXIT_CRITICAL(&_moduleCommandLock);


        if (_moduleCommandHandler != nullptr)
        {
            DEBUG_VALUE("[HomeShield] Module command dispatched", command);

            _moduleCommandHandler(String(command));
        }
        else
        {
            DEBUG_LOG(
                "[HomeShield] A module command was queued but this sketch has "
                "no module command handler.");
        }
    }


    /*
     * Heartbeat.
     */
    if (MqttConnected())
    {
        if (millis() -
            _lastHeartbeat >=
            _heartbeatInterval)
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
        DEBUG_LOG(
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

    DEBUG_LOG(
        "[HomeShield] Provisioning reset button held for 5 s.");

    // Milestone 40. The same code the network UNPROVISION runs, deliberately.
    // The button and the server command are two ways of asking for one thing,
    // and a board that ended up in a different state depending on which was
    // used would be a state nobody designed.
    Unprovision();
}


void HomeShieldClass::setHeartbeatInterval(
    unsigned long milliseconds)
{
    if (milliseconds == 0)
    {
        DEBUG_LOG(
            "[HomeShield] A heartbeat interval of 0 was ignored. A board that "
            "never heartbeats is a board the Control Server declares offline.");

        return;
    }


    _heartbeatInterval = milliseconds;
}


void HomeShieldClass::setCommandHandler(
    CommandHandler handler)
{
    _commandHandler =
        handler;
}


void HomeShieldClass::setModuleCommandHandler(
    ModuleCommandHandler handler)
{
    _moduleCommandHandler =
        handler;
}


// ==================================================
// Module capabilities (milestone 38)
// ==================================================

bool HomeShieldClass::declareModuleCapability(
    const String& capabilityKey)
{
    // Refused after the module has started, for exactly the reason addDevice()
    // is: the registration task reads this array as it stands the moment it is
    // created, so a late declaration would be sent on some registrations and
    // not others depending on timing.
    if (_started)
    {
        DEBUG_LOG(
            "[HomeShield] declareModuleCapability() was called after the module "
            "had already started. It is ignored. Declare capabilities BEFORE "
            "begin()/beginModule().");

        return false;
    }


    if (capabilityKey.length() == 0)
    {
        DEBUG_LOG(
            "[HomeShield] declareModuleCapability() refused: the capability key "
            "is empty.");

        return false;
    }


    for (int i = 0; i < _capabilityCount; i++)
    {
        // Already declared. Not an error - a sketch restructured so that two
        // components both assert the same capability is fine, and refusing it
        // would make the order they run in matter.
        if (_capabilities[i] == capabilityKey) return true;
    }


    if (_capabilityCount >= MAX_CAPABILITIES)
    {
        DEBUG_LOG_PRINT(
            "[HomeShield] declareModuleCapability() refused: this library "
            "declares at most ");

        DEBUG_LOG_PRINT(MAX_CAPABILITIES);

        DEBUG_LOG_PRINT(
            " capabilities per module, and '");

        DEBUG_LOG_PRINT(capabilityKey);

        DEBUG_LOG(
            "' would exceed it.");

        return false;
    }


    _capabilities[_capabilityCount] =
        capabilityKey;

    _capabilityCount++;

    return true;
}


// ==================================================
// Gaining a device at runtime (milestone 38)
// ==================================================

bool HomeShieldClass::addDeviceAtRuntime(
    const String& deviceKey,
    const String& deviceType,
    const String& defaultName)
{
    if (!_started ||
        _registrationService == nullptr)
    {
        DEBUG_LOG(
            "[HomeShield] addDeviceAtRuntime() was called before the module "
            "started. Use addDevice() during setup instead.");

        return false;
    }


    if (deviceKey.length() == 0 ||
        deviceType.length() == 0)
    {
        DEBUG_LOG(
            "[HomeShield] addDeviceAtRuntime() refused: a device needs both a "
            "device key and a device type.");

        return false;
    }


    // Already declared. Refused rather than treated as success: the caller is
    // about to persist a node on the strength of this, and a key collision
    // means the thing it thinks it adopted is not the thing that is there.
    if (IndexOf(deviceKey) >= 0)
    {
        DEBUG_LOG_PRINT(
            "[HomeShield] addDeviceAtRuntime() refused: device key '");

        DEBUG_LOG_PRINT(deviceKey);

        DEBUG_LOG(
            "' is already declared.");

        return false;
    }


    if (_deviceCount >= MAX_DEVICES)
    {
        DEBUG_LOG_PRINT(
            "[HomeShield] addDeviceAtRuntime() refused: this library declares "
            "at most ");

        DEBUG_LOG_PRINT(MAX_DEVICES);

        DEBUG_LOG(
            " devices per module.");

        return false;
    }


    // Written BEFORE the count moves. The registration task reads the array
    // through a pointer it already holds, so a count that advanced first would
    // expose a half-written slot for as long as the next three assignments take.
    _devices[_deviceCount].deviceKey = deviceKey;
    _devices[_deviceCount].deviceType = deviceType;
    _devices[_deviceCount].defaultName = defaultName;


    _reRegistering = true;

    _reRegistrationResult =
        ReRegistration::InProgress;


    // Publishes the new count under the service's own lock and clears the
    // registered flag, so the task re-registers within a second and the outcome
    // that follows belongs to THIS declaration.
    _registrationService->Redeclare(
        _deviceCount + 1);


    _deviceCount++;


    return true;
}


// ==================================================
// Losing a device at runtime (milestone 42)
// ==================================================

bool HomeShieldClass::removeDeviceAtRuntime(
    const String& deviceKey)
{
    if (!_started ||
        _registrationService == nullptr)
    {
        DEBUG_LOG(
            "[HomeShield] removeDeviceAtRuntime() was called before the module "
            "started.");

        return false;
    }


    int index = IndexOf(deviceKey);

    if (index < 0)
    {
        DEBUG_VALUE(
            "[HomeShield] removeDeviceAtRuntime() refused: no such device key: ",
            deviceKey);

        return false;
    }


    // COMPACTED FIRST, count lowered second - the opposite order to
    // addDeviceAtRuntime(), and for the same underlying reason.
    //
    // The registration task reads this array through a pointer it already
    // holds, but only while it is trying to register: once _registered is true
    // it does not touch it again until something clears the flag. Redeclare()
    // is what clears it, so compacting before that call means the task cannot
    // observe a half-shifted array. Lowering the count first would expose a
    // window in which the list still contained the removed key and had lost
    // the last one instead.
    for (int i = index; i < _deviceCount - 1; i++)
    {
        _devices[i] = _devices[i + 1];
    }

    // The vacated tail slot is cleared rather than left holding the last
    // device's strings. Nothing reads past the count, but a stale duplicate
    // sitting there is exactly the kind of thing a future reader trusts.
    _devices[_deviceCount - 1] = DeclaredDevice{};


    _reRegistering = true;

    _reRegistrationResult =
        ReRegistration::InProgress;


    // Publishes the new count under the service's own lock and clears the
    // registered flag, so the task re-registers within a second with a
    // declaration that no longer mentions this child.
    _registrationService->Redeclare(
        _deviceCount - 1);


    _deviceCount--;


    DEBUG_VALUE(
        "[HomeShield] Device removed at runtime: ",
        deviceKey);

    return true;
}


HomeShieldClass::ReRegistration
HomeShieldClass::reRegistrationState()
{
    if (_registrationService == nullptr)
    {
        return ReRegistration::Idle;
    }


    // Settled already. Held rather than recomputed, because the counters it was
    // derived from keep moving - a hub that reconnects later would otherwise
    // see a long-finished failure turn into a success.
    if (!_reRegistering)
    {
        return _reRegistrationResult;
    }


    if (_registrationService->IsRegistered())
    {
        _reRegistering = false;

        _reRegistrationResult =
            ReRegistration::Succeeded;

        return _reRegistrationResult;
    }


    int statusCode =
        _registrationService->LastStatusCode();


    // A 4xx is the Control Server refusing the declaration itself - an unknown
    // device type, a bad key, a duplicate. Retrying cannot fix it, so it fails
    // now rather than in thirty seconds.
    bool refused =
        statusCode >= 400 &&
        statusCode < 500;


    if (refused ||
        _registrationService->FailedAttempts() >=
            MAX_REREGISTRATION_FAILURES)
    {
        _reRegistering = false;

        _reRegistrationResult =
            ReRegistration::Failed;

        return _reRegistrationResult;
    }


    return ReRegistration::InProgress;
}


int HomeShieldClass::lastRegistrationStatus()
{
    if (_registrationService == nullptr) return 0;

    return _registrationService->LastStatusCode();
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


    // --------------------------------------------------
    // Milestone 40: which topic did this arrive on?
    // --------------------------------------------------
    //
    // The topic was ignored until now, and correctly so - there was exactly
    // one subscription and every message on it was a command. There are two
    // now, and they mean entirely different things, so the routing is done
    // HERE rather than by inspecting the payload: a message is unprovisioning
    // because of where it was published, not because of what it happens to
    // say. A device command that contained the word UNPROVISION must reach the
    // sketch's handler untouched.
    String topicText = topic != nullptr ? String(topic) : String();

    if (topicText.endsWith(PROVISIONING_COMMAND_SUFFIX))
    {
        // The whole payload contract: {"command":"UNPROVISION"}. Anything else
        // on this topic is refused rather than guessed at - the one action it
        // can ask for is irreversible.
        String command;

        if (!JsonLite::ReadString(message, "command", command) ||
            command != UNPROVISION_COMMAND)
        {
            DEBUG_VALUE("[HomeShield] Ignored an unrecognised provisioning command", message);

            return;
        }


        DEBUG_LOG(
            "[HomeShield] UNPROVISION received. Clearing provisioning after "
            "this MQTT callback returns.");

        // Recorded only. loop() does the clearing and the restart; see
        // _unprovisionRequested.
        _instance->_unprovisionRequested = true;

        return;
    }


    _instance->Dispatch(
        message);
}


// ==================================================
// Physical unprovisioning (milestone 40)
// ==================================================
//
// The counterpart of the five-second reset button, reached over the network
// instead of with a finger, and it does the SAME thing by design rather than
// by coincidence: one definition of "unprovisioned" and one piece of code that
// produces it. Anything this path cleared that the button did not would be a
// state no household could ever reach by hand.
//
// What is cleared: the Wi-Fi credentials, the Control Server URL, the
// provisioning commit marker, and - on a Sensor Hub - the adopted-node
// registry, which StorageService clears with them (A38-8).
//
// What is NOT touched, and cannot be: the factory MAC. It is eFuse, it is this
// board's permanent identity, and nothing in HomeShield writes it. The
// firmware is not touched either; the board reboots into the same sketch and
// picks up M37's BLE onboarding because its NVS no longer says otherwise.
//
// There is no reply. The Control Server is not waiting for one and, by the
// time this runs, has already deleted or is deleting the records this board
// would be replying about.
void HomeShieldClass::Unprovision()
{
    DEBUG_LOG(
        "[HomeShield] Unprovisioning: clearing the HomeShield configuration.");


    bool cleared =
        _storageService.ClearProvisioningConfig();


    if (!cleared)
    {
        // Reported, and then the restart happens anyway.
        //
        // The board cannot stay as it is: the household has deleted it and the
        // Control Server will refuse everything it sends from here on. A reboot
        // that comes back still provisioned is at least a board that says so on
        // its serial port, which is the only thing that tells somebody it needs
        // erasing by hand. Refusing to reboot would leave it running against a
        // server that has forgotten it, silently.
        DEBUG_LOG(
            "[HomeShield] WARNING: the provisioning configuration could NOT be "
            "verified as cleared. Restarting anyway - if this board comes back "
            "provisioned, its NVS must be erased manually.");
    }


    // The Wi-Fi driver keeps its own copy of the credentials in its own NVS
    // namespace, which HomeShield does not own and ClearProvisioningConfig()
    // does not touch. Erasing it here is what stops the board silently
    // rejoining the household's network on the next boot while advertising
    // itself for onboarding. Exactly what CheckResetButton() does.
    WiFi.disconnect(true, true);

    delay(100);


    DEBUG_LOG(
        "[HomeShield] Unprovisioned. Restarting into BLE onboarding.");

    Serial.flush();

    ESP.restart();
}


void HomeShieldClass::Dispatch(
    const String& payload)
{
    // --------------------------------------------------
    // The guard is PER PATH, and that is the fix
    // --------------------------------------------------
    //
    // This used to be `if (_commandHandler == nullptr) return;` - a single
    // check, at the top, written when a command could only ever be for a
    // device.
    //
    // A Sensor Hub sets setModuleCommandHandler() and NOTHING else, because in
    // milestone 38 it has no child devices to command. So _commandHandler was
    // null on every hub, and every module command was discarded on the first
    // line of this function - before the scope check below could see it, and
    // before anything printed. The board logged "Command Received" from the
    // MQTT callback and then went silent, which is exactly what it looked like.
    //
    // Bail only when there is no handler of EITHER kind. Each path checks its
    // own handler where it is actually used.
    if (_commandHandler == nullptr &&
        _moduleCommandHandler == nullptr)
    {
        return;
    }


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


        // --------------------------------------------------
        // Milestone 38: module scope
        // --------------------------------------------------
        //
        // Read BEFORE any device is resolved, and it returns rather than
        // falling through, because a module-scoped command is not about a
        // device and there is nothing below that could do anything sensible
        // with it. A Sensor Hub holds zero children when it is new and several
        // later, so both of the device path's outcomes - "no such key" and "no
        // single device to fall back to" - would simply drop it.
        //
        // Anything WITHOUT scope == "module" continues past here exactly as it
        // always has. That is the whole compatibility story: every message any
        // existing board has ever received is missing this field, takes the
        // branch below, and behaves identically.
        String scope;

        if (JsonLite::ReadString(body, "scope", scope) &&
            scope == "module")
        {
            if (_moduleCommandHandler == nullptr)
            {
                DEBUG_VALUE(
                    "Dropped a module command: this sketch has no module "
                    "command handler. Command was: ",
                    command);

                return;
            }

            // QUEUED, not called. See _moduleCommandInbox: running it here
            // would publish from inside PubSubClient's own receive callback,
            // into the buffer PubSubClient is still reading.
            if (command.length() > MAX_MODULE_COMMAND)
            {
                DEBUG_LOG_PRINT(
                    "[HomeShield] Dropped a module command longer than ");

                DEBUG_LOG_PRINT(MAX_MODULE_COMMAND);

                DEBUG_LOG(" characters.");

                return;
            }

            portENTER_CRITICAL(&_moduleCommandLock);

            strncpy(
                _moduleCommandInbox,
                command.c_str(),
                MAX_MODULE_COMMAND);

            _moduleCommandInbox[MAX_MODULE_COMMAND] = '\0';

            _hasModuleCommand = true;

            portEXIT_CRITICAL(&_moduleCommandLock);


            DEBUG_VALUE("[HomeShield] Module command queued", command);

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


    // Checked HERE rather than at the top of the function: a module-only
    // sketch legitimately has no device command handler, and a device command
    // arriving at one is worth saying out loud rather than silently dropping.
    if (_commandHandler == nullptr)
    {
        DEBUG_VALUE(
            "Dropped a device command: this sketch has no device command "
            "handler. Command was: ",
            command);

        return;
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
        DEBUG_LOG(
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


bool HomeShieldClass::publishModuleEvent(
    const String& eventType,
    const String& payloadJson)
{
    // The private, key-taking overload with an EMPTY key, which is what omits
    // the deviceKey field from the envelope. The Control Server routes this by
    // hardwareId and eventType; no device is named because none is meant.
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
