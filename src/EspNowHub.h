#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "EspNowProtocol.h"

// ============================================================
// EspNowHub - the ESP32-S3 half of onboarding (milestone 38)
// ============================================================
//
// A Sensor Hub is an ordinary HomeShield module: it registers as
// esp32-generic, joins Wi-Fi, talks MQTT and sends the framework
// heartbeat, all through HomeShieldClass and none of it
// reimplemented here. What this class adds is the ability to
// ADOPT sensor nodes over ESP-NOW and turn each one into a child
// Device.
//
// It holds two things the hub cannot do without:
//
//   1. A persistent node registry in NVS. The hub re-declares
//      every adopted node at boot, because the Control Server
//      reads a registration as the complete truth about a
//      module's children and DISABLES any child it stops hearing
//      about. An adopted node is silent - a provisioned node does
//      nothing at all in M38 - so it cannot be asked again. The
//      registry is the only thing that remembers.
//
//   2. One discovery session at a time, in RAM. It is deliberately
//      not persisted: a session describes a user standing in front
//      of a sensor, and a "discovering" row that outlived a
//      restart would describe nothing.
//
// ------------------------------------------------------------
// Channels
// ------------------------------------------------------------
//
// The hub NEVER changes channel. It is joined to the household
// router and moving would drop Wi-Fi, MQTT, the Control Server
// and its heartbeat. It broadcasts DISCOVERY_REQUEST every 500 ms
// on whatever channel the router put it on; the node sweeps to
// find it. See EspNowNode.h for the other half of that argument.
//
// ------------------------------------------------------------
// What is NOT here
// ------------------------------------------------------------
//
// No child commands, no child state, no child events, no child
// heartbeat, and no way to remove a node. All milestone 39. The
// msgType space above 4 and the record format's version key are
// where those arrive.
// ============================================================

class EspNowHubClass
{
public:

    // --------------------------------------------------
    // Setup, part 1 - BEFORE beginModule()
    // --------------------------------------------------
    //
    // Loads the registry and declares one device per adopted node
    // through HomeShield.addDevice().
    //
    // The ordering is not a style choice. addDevice() is refused
    // once the module has started, and the first registration a
    // module sends is read as the whole truth about its children -
    // so a hub that started before declaring its nodes would have
    // every one of them disabled by the Control Server.
    //
    // Returns how many nodes were declared.
    int declarePersistedNodes();


    // --------------------------------------------------
    // Setup, part 2 - AFTER beginModule()
    // --------------------------------------------------
    //
    // Brings up ESP-NOW and the broadcast peer. Does nothing while
    // the board is unprovisioned (milestone 37 state): there is no
    // Control Server to ask for a discovery and no settled channel
    // to sit on, so it waits and starts once Wi-Fi is up.
    void begin();


    void loop();


    // --------------------------------------------------
    // Module commands
    // --------------------------------------------------
    //
    // Fed from the sketch's ModuleCommandHandler. Understands:
    //
    //   DISCOVER_NODE:70AF0935923C
    //   CANCEL_NODE_DISCOVERY
    //
    // Anything else is ignored and said so on the serial port.
    // Returns true when the command was recognised.
    bool handleModuleCommand(
        const String& command);


    int NodeCount() const;


private:

    enum class State
    {
        // Registered, connected, heartbeating. Not looking for
        // anything. The resting state.
        Normal,

        // Broadcasting for one specific node MAC. There is NO
        // timeout: this ends when the node answers, when the user
        // cancels, or when the hub restarts.
        Discovering,

        // The node answered and its identity checked out.
        NodeFound,

        // The node confirmed this hub and wrote our MAC.
        IdentityConfirmed,

        // Re-registering with the node as a new child device.
        // Cancellation is IGNORED here - see loop().
        Registering,
    };


    struct NodeRecord
    {
        String mac;          // 12 uppercase hex
        String deviceType;
        String deviceKey;    // the node's KIND key, e.g. "door"
        String defaultName;
    };


    void StartRadio();

    void Broadcast();

    void ReportStatus();

    void HandleResponse(
        const HsDiscoveryResponse& response);

    void HandleConfirm(
        const HsIdentityConfirm& confirm);

    void SendAck(
        bool accepted,
        uint8_t reason);

    bool AddNodePeer();

    void DropNodePeer();


    void StartDiscovery(
        const String& targetMacText);

    void CancelDiscovery();

    void BeginRegistration();

    void PollRegistration();


    // Ends the session and returns to Normal. Persists nothing.
    void EndSession(
        const char* phase,
        const char* failureCode);


    // One module-scoped NodeDiscovery event, as designed in §16.5.
    void Report(
        const char* phase,
        const char* failureCode);


    // "door" + 70AF0935923C -> "door-70af0935923c". The
    // module-local address the Control Server reconciles on, and
    // the reason a re-flashed hub keeps every child's Device.Id:
    // it is derived only from values the registry already holds,
    // so it is byte-identical on every boot.
    static String ComposeDeviceKey(
        const String& deviceKey,
        const String& mac);


    bool IsAdopted(
        const String& mac) const;


    bool LoadRegistry();

    bool AppendToRegistry(
        const NodeRecord& record);


    static String EncodeRecord(
        const NodeRecord& record);

    static bool DecodeRecord(
        const String& text,
        NodeRecord& record);


public:

    // Called from the ESP-NOW receive callback on the Wi-Fi task.
    // Records only; loop() does the work, because publishing to
    // MQTT and writing NVS are neither fast nor reentrant.
    void OnFrameReceived(
        const uint8_t* mac,
        const uint8_t* data,
        int length);


    // Called from the ESP-NOW send callback. Says a frame actually left the
    // radio, as opposed to having been accepted for transmission.
    void OnFrameSent(
        bool ok);


private:

    State _state =
        State::Normal;


    bool _radioStarted = false;

    unsigned long _lastRadioAttemptAt = 0;

    static constexpr unsigned long RadioRetryInterval = 2000;


    uint8_t _hubMac[6] = {0};


    // --------------------------------------------------
    // The live session (RAM only)
    // --------------------------------------------------

    uint8_t _targetMac[6] = {0};

    String _targetMacText;

    uint32_t _sessionId = 0;

    uint8_t _sequence = 0;

    bool _nodePeerAdded = false;

    // What the node said about itself, held from NODE_FOUND until
    // the registry append at the very end.
    NodeRecord _pending;

    String _pendingComposedKey;


    unsigned long _lastBroadcastAt = 0;

    // EspNowNode::ChannelDwell MUST stay above this, or a sweeping node can
    // be on this channel and still miss every frame.
    static constexpr unsigned long BroadcastInterval = 500;


    // --------------------------------------------------
    // Diagnostics
    // --------------------------------------------------
    //
    // Added during bring-up, when a discovery that silently did nothing could
    // not be told apart from one that was transmitting into an empty room.

    uint32_t _broadcastsSent = 0;

    uint32_t _broadcastsFailed = 0;

    uint32_t _sendsCompleted = 0;

    uint32_t _sendsFailed = 0;

    bool _loopConfirmed = false;

    // Every ESP-NOW frame delivered to this hub, from anyone, valid or not.
    uint32_t _framesHeard = 0;

    // Said once, so a rejected frame is visible without flooding the log.
    bool _reportedForeignFrame = false;

    // Why a DISCOVERY_RESPONSE was refused, reported once each per session.
    uint32_t _droppedWrongSession = 0;

    uint32_t _droppedWrongNode = 0;

    uint32_t _droppedWrongHub = 0;

    unsigned long _lastStatusAt = 0;

    static constexpr unsigned long StatusInterval = 5000;


    unsigned long _lastAckAt = 0;

    int _ackAttempts = 0;

    static constexpr unsigned long AckInterval = 1000;

    static constexpr int MaxAckAttempts = 5;


    // --------------------------------------------------
    // Registry
    // --------------------------------------------------
    //
    // 12 nodes fits inside HomeShieldClass::MAX_DEVICES (16) with
    // headroom for the hub to gain a device of its own later.
    static constexpr int MAX_NODES = 12;

    NodeRecord _nodes[MAX_NODES];

    int _nodeCount = 0;


    // --------------------------------------------------
    // Inbox
    // --------------------------------------------------

    static constexpr size_t MaxFrame = 128;

    portMUX_TYPE _inboxLock =
        portMUX_INITIALIZER_UNLOCKED;

    uint8_t _inbox[MaxFrame] = {0};

    volatile int _inboxLength = 0;

    volatile bool _hasInbox = false;


    Preferences _preferences;


    // --------------------------------------------------
    // NVS (namespace "hs_nodes")
    // --------------------------------------------------
    //
    // reg_state is the commit marker, written last. A record is
    // written, verified, THEN the count moves, THEN the marker - so
    // a power cut leaves an unreferenced half-record rather than a
    // count that points at nothing.
    //
    // All four fields of a record are stored and all four are
    // required: the hub re-declares a child at boot without the
    // node being present, and a MAC alone cannot produce a device
    // type, a key or a name.
    //
    // StorageService::ClearProvisioningConfig() clears this whole
    // namespace (decision A38-8): a hub being re-onboarded is
    // starting over.

    static constexpr const char* Namespace = "hs_nodes";

    static constexpr const char* VersionKey = "reg_ver";

    static constexpr const char* CountKey = "count";

    static constexpr const char* StateKey = "reg_state";

    static constexpr int CurrentVersion = 1;
};


extern EspNowHubClass EspNowHub;
