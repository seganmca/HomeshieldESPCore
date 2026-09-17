#pragma once

#include <Arduino.h>
#include <DeviceIdentity.h>
#include <DeviceKeys.h>
#include <DeclaredDevice.h>
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
