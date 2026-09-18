#pragma once

#include <Arduino.h>
#include <DeviceIdentity.h>
#include <DeviceKeys.h>
#include <DeclaredDevice.h>
#include <ModuleCapabilities.h>
#include <StorageService.h>
#include <HttpService.h>
#include <RegistrationService.h>
#include <ProvisioningService.h>
#include <MqttService.h>


// --------------------------------------------------
// Command handler
// --------------------------------------------------
//
// Milestone 36 phase 3. The handler now receives the DeviceKey of
// the device the command is for, because one controller can hold
// many.
//
// deviceKey is always a REAL declared key, never empty. A command
// from the Control Server to a single-device module still arrives
// as a bare string with no key on the wire - that is what keeps
// existing boards working - and the library resolves it to that
// module's only declared device before calling here. So a
// single-device sketch receives DeviceKeys::Main and can simply
// ignore the argument.

typedef void (*CommandHandler)(
    const String& deviceKey,
    const String& command);


// --------------------------------------------------
// Module command handler (milestone 38)
// --------------------------------------------------
//
// A command addressed to the CONTROLLER rather than to one of the
// devices on it.
//
// It exists because a Sensor Hub could not be reached any other
// way. Dispatch() resolves an inbound command to a declared
// device and drops it when it cannot; a hub with zero children -
// which is how every hub starts - has nothing to resolve to, and
// a hub with several has nothing to fall back to. "Start looking
// for node 70AF0935923C" is not about a device in either case.
//
// It arrives on the SAME MQTT topic, in an envelope that says so:
//
//   {"scope":"module","command":"DISCOVER_NODE:70AF0935923C"}
//
// Anything without scope == "module", including every message
// sent before this milestone, falls through to the device path
// byte for byte. No new topic and no new subscription.

typedef void (*ModuleCommandHandler)(
    const String& command);


// --------------------------------------------------
// Device Telemetry
// --------------------------------------------------
//
// batteryLevel:
//   0-100 = valid battery percentage
//   -1    = not available
//
// wifiStrength:
//   WiFi RSSI in dBm
//   Example: -45, -62, -78
//
// WiFi strength is collected automatically by
// HomeShield. The device only needs to provide
// battery level if it has a battery.
//
// Milestone 36: both are MODULE facts. A battery powers a board
// and a radio belongs to a board, however many devices hang off
// it, so these stay controller-level and are reported once on the
// module's heartbeat.
// --------------------------------------------------

struct DeviceTelemetry
{
    int batteryLevel = -1;
};


class HomeShieldClass
{
public:

    // ==================================================
    // Starting up - call exactly ONE of these
    // ==================================================

    // --------------------------------------------------
    // Single-device controller
    // --------------------------------------------------
    //
    // The original entry point, with its original signature and
    // its original meaning. Every existing sketch calls this and
    // none of them needed editing.
    //
    // It declares one device for you, keyed DeviceKeys::Main, and
    // declares no module type - the Control Server derives the
    // module type from that one device, which is exactly what it
    // already did for every board registered before milestone 36.
    //
    // That is deliberate and it is the safety property of the whole
    // phase: because the key comes from the library, a single-device
    // sketch cannot get it wrong, and its Device.Id survives the
    // reflash.
    void begin(
        const String& deviceType,
        const String& firmwareVersion);


    // --------------------------------------------------
    // Multi-device controller
    // --------------------------------------------------
    //
    // Declare each device with addDevice(), then start with
    // beginModule(). A board with ten relays is ONE module holding
    // ten switch devices:
    //
    //   HomeShield.addDevice("relay1", DeviceTypes::Light, "Channel 1");
    //   HomeShield.addDevice("relay2", DeviceTypes::Light, "Channel 2");
    //   HomeShield.beginModule(ModuleTypes::RelayBoard, "2.0.0");
    //
    // deviceKey is the module-local routing address: lowercase
    // letters, digits, '-' and '_', at most 32 characters. It is
    // not an identity and never reaches a user.
    //
    // Declaration must be COMPLETE before beginModule(); a call
    // afterwards is refused.
    void addDevice(
        const String& deviceKey,
        const String& deviceType,
        const String& defaultName = "");


    // moduleType comes from ModuleTypes.h. It is presentational -
    // an unrecognised one still registers and is recorded as
    // "esp32-generic".
    //
    // Named beginModule rather than overloading begin() because a
    // device type and a module type are the same C++ type
    // (const char*), so the two could not be told apart by
    // signature - the compiler would reject the declaration
    // outright, and forcing an overload would silently resolve both
    // to whichever one came first.
    void beginModule(
        const String& moduleType,
        const String& firmwareVersion);


    void loop();


    // --------------------------------------------------
    // Provisioning reset button (milestone 37)
    // --------------------------------------------------
    //
    // The board's dedicated, externally accessible reset button - never the
    // BOOT button. Call before begin()/beginModule(). Holding it for 5 s
    // clears the provisioning configuration (Wi-Fi and Control Server URL)
    // and restarts the board into BLE onboarding. The hardware identity is
    // the eFuse MAC and is not touched.
    //
    // pin -1, or never calling this, leaves the board without a reset.
    void setProvisioningResetButton(
        int pin,
        bool activeLow = true);


    void setCommandHandler(
        CommandHandler handler);


    // Milestone 38. Receives module-scoped commands; see
    // ModuleCommandHandler. May be set at any time.
    void setModuleCommandHandler(
        ModuleCommandHandler handler);


    // ==================================================
    // Module capabilities (milestone 38)
    // ==================================================

    // --------------------------------------------------
    // Declare what this module can DO
    // --------------------------------------------------
    //
    // Keys come from ModuleCapabilities.h. Call before
    // begin()/beginModule(); a call afterwards is refused, for the
    // same reason addDevice() is - the registration body is built
    // from this array the moment the registration task starts.
    //
    // A capability is not a type. A type is presentational and
    // decides nothing (M36 D2); this decides whether the Control
    // Server will accept a node discovery for this module, so it
    // had to be something a board asserts rather than something
    // inferred from how it looks.
    //
    // Declaring none - which every board before M38 does - sends
    // no capabilities field at all.
    bool declareModuleCapability(
        const String& capabilityKey);


    // ==================================================
    // Gaining a device at runtime (milestone 38)
    // ==================================================

    // --------------------------------------------------
    // Declare a device AFTER the module has started
    // --------------------------------------------------
    //
    // The one door into the otherwise-frozen declaration, and it
    // is deliberately narrow: a Sensor Hub that has just adopted a
    // sensor node needs that node to become a child Device, and
    // registration is the only thing that creates one.
    //
    // It appends to the declared array and asks RegistrationService
    // to run again, which POSTs the WHOLE declaration - every
    // device already declared plus this one. That is required
    // rather than wasteful: the Control Server reads a registration
    // as the complete truth about a module's children and disables
    // any child it stops hearing about.
    //
    // Returns false and changes nothing if the key is already
    // declared, the key or type is empty, the table is full, or the
    // module has not started. The caller must then NOT persist the
    // node - a node the Control Server never accepted is not
    // adopted.
    //
    // Success here means the declaration was ACCEPTED, not that it
    // registered. Poll reRegistrationState() for that.
    bool addDeviceAtRuntime(
        const String& deviceKey,
        const String& deviceType,
        const String& defaultName = "");


    // --------------------------------------------------
    // How the re-registration is going
    // --------------------------------------------------
    //
    //   Idle        nothing has been re-declared this boot
    //   InProgress  declared, waiting for the Control Server
    //   Succeeded   registered; the child Device exists
    //   Failed      refused, or the retries were exhausted
    //
    // Failed is decided the same way milestone 37's provisioning
    // commit decides it: three consecutive failures, or a 4xx,
    // which will not improve by being retried.
    enum class ReRegistration
    {
        Idle,
        InProgress,
        Succeeded,
        Failed,
    };

    ReRegistration reRegistrationState();


    // The HTTP status of the last registration attempt, or 0 when no
    // response arrived. It is what tells a refused declaration (4xx -
    // the sketch is wrong) from an unreachable Control Server (0 -
    // the network is), which are different failures and read
    // differently to a household.
    int lastRegistrationStatus();


    // ==================================================
    // Reporting state and events
    // ==================================================
    //
    // Milestone 36 phase 3. The library owns the event envelope.
    // It used to be hand-built identically in seven sketches, which
    // meant seven copies of the wire format to keep in step.

    // --------------------------------------------------
    // Single-device controller
    // --------------------------------------------------
    //
    // Sends no DeviceKey at all, which is correct AND deliberate:
    // the Control Server resolves a keyless message to the module's
    // only device, so the envelope stays byte-identical to what
    // these boards have always published - and a key that were
    // present and somehow did not match would be dropped with no
    // fallback. Refused (returning false) if more than one device
    // is declared, because then there is no "the" device.
    bool publishState(
        int state);

    bool publishEvent(
        const String& eventType,
        const String& payloadJson);


    // --------------------------------------------------
    // Multi-device controller
    // --------------------------------------------------
    //
    // The DeviceKey is mandatory here. There is no single device to
    // fall back to, and guessing which relay a report meant would be
    // worse than dropping it.
    bool publishState(
        const String& deviceKey,
        int state);

    bool publishEvent(
        const String& deviceKey,
        const String& eventType,
        const String& payloadJson);


    // --------------------------------------------------
    // Module-scoped events (milestone 38)
    // --------------------------------------------------
    //
    // An event about the BOARD rather than about any device on it,
    // sent with no DeviceKey however many devices are declared.
    //
    // The keyless publishEvent() above cannot serve this: it is
    // the SINGLE-DEVICE convenience and refuses a module holding
    // more than one, because there a keyless report is ambiguous.
    // This one is not ambiguous - it is explicitly about the
    // module - so it is a separate method rather than a relaxation
    // of that rule.
    //
    // A Sensor Hub reporting how an onboarding is going is the
    // first caller, and it must work when the hub holds zero
    // devices and when it holds six.
    bool publishModuleEvent(
        const String& eventType,
        const String& payloadJson);


    // Raw publish to an arbitrary topic. Retained unchanged; the
    // envelope is yours to build if you use it.
    bool publish(
        const String& topic,
        const String& message);


    String GetHardwareId();


    bool IsConnected() const;


    bool MqttConnected();


    // --------------------------------------------------
    // Telemetry
    // --------------------------------------------------

    void SetBatteryLevel(
        int batteryLevel);


private:

    // --------------------------------------------------
    // Declared devices
    // --------------------------------------------------
    //
    // Fixed size, no dynamic allocation: ten channels for the relay
    // board this milestone exists for, plus headroom. A declaration
    // beyond this is refused with a message rather than silently
    // truncated.
    static constexpr int MAX_DEVICES = 16;

    DeclaredDevice _devices[MAX_DEVICES];

    int _deviceCount = 0;


    // Set once one of begin()/beginModule() has accepted the
    // declaration. Nothing registers, connects or publishes until
    // it is true.
    bool _started = false;


    // --------------------------------------------------
    // Declared capabilities (milestone 38)
    // --------------------------------------------------
    //
    // Fixed size and small on purpose: a module asserting more than
    // four things about itself is a design that wants revisiting,
    // not a bigger array.
    static constexpr int MAX_CAPABILITIES = 4;

    String _capabilities[MAX_CAPABILITIES];

    int _capabilityCount = 0;


    // --------------------------------------------------
    // Runtime re-registration (milestone 38)
    // --------------------------------------------------

    bool _reRegistering = false;

    ReRegistration _reRegistrationResult =
        ReRegistration::Idle;

    // Consecutive failures tolerated before a re-registration is
    // called lost. The registration task retries every 10 s, so
    // this is roughly a 30-second verdict - the same budget M37's
    // provisioning commit uses.
    static constexpr int MAX_REREGISTRATION_FAILURES = 3;


    // The shared tail of begin() and beginModule().
    void Start(
        const String& moduleType,
        const String& firmwareVersion);


    // Index of a declared device by key, or -1.
    int IndexOf(
        const String& deviceKey) const;


    // Resolves an inbound command to a declared device and calls the
    // handler. THE ONE PLACE the keyless legacy path lives.
    void Dispatch(
        const String& payload);


    unsigned long _lastHeartbeat = 0;


    static constexpr unsigned long HEARTBEAT_INTERVAL =
        60 * 1000;


    // The topic devices report state changes and heartbeats on.
    // UNCHANGED from every previous milestone - it simply lives in
    // one place now that the library builds the envelope.
    static constexpr const char* EVENTS_TOPIC =
        "homeshield/events";


    String _firmwareVersion;

    String _moduleType;


    StorageService _storageService;


    HttpService _httpService;


    RegistrationService*
        _registrationService = nullptr;


    ProvisioningService*
        _provisioningService = nullptr;


    MqttService _mqttService;


    CommandHandler
        _commandHandler = nullptr;


    ModuleCommandHandler
        _moduleCommandHandler = nullptr;


    // --------------------------------------------------
    // Module command inbox (milestone 38 bring-up fix)
    // --------------------------------------------------
    //
    // A module command is QUEUED here by Dispatch() and executed by loop().
    // It used to be executed inline, and that was a real defect rather than a
    // style point.
    //
    // Dispatch() runs inside PubSubClient's message callback, which is called
    // from _mqttClient.loop() while PubSubClient is still parsing the packet it
    // just read. PubSubClient uses ONE buffer for receive and transmit, so a
    // publish from inside that callback overwrites the very bytes being parsed.
    // A Sensor Hub's first act on DISCOVER_NODE is to publish its "Discovering"
    // phase - so the handler was reaching straight back into the client that
    // was calling it.
    //
    // This is the same arrangement ProvisioningService already uses for BLE:
    // the callback only records, and Loop() does the work. One slot is enough -
    // a second command arriving before the first is drained is either a
    // duplicate or a cancel, and the newer one is the one worth keeping.
    static constexpr size_t MAX_MODULE_COMMAND = 96;

    portMUX_TYPE _moduleCommandLock =
        portMUX_INITIALIZER_UNLOCKED;

    char _moduleCommandInbox[MAX_MODULE_COMMAND + 1] = {0};

    volatile bool _hasModuleCommand = false;


    bool _mqttInitialized = false;


    // Milestone 37 (D1). The broker host is the Control Server URL's host.
    // Held here because MqttService keeps the pointer it is given.
    String _mqttHost;


    // Milestone 37. The provisioning-reset button.
    int _resetPin = -1;

    bool _resetActiveLow = true;

    unsigned long _resetPressedAt = 0;

    bool _resetPressed = false;

    static constexpr unsigned long RESET_HOLD_TIME =
        5000;

    void CheckResetButton();

    bool _wasWifiConnected = false;


    DeviceTelemetry _telemetry;


    static HomeShieldClass*
        _instance;


    static void OnMqttMessage(
        char* topic,
        byte* payload,
        unsigned int length);


    void publishHeartbeat();
};


extern HomeShieldClass HomeShield;
