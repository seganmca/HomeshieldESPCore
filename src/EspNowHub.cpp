#include "EspNowHub.h"
#include "HomeShield.h"
#include "Debug.h"
#include "DeviceIdentity.h"
#include "DeviceEventTypes.h"
#include "JsonLite.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_random.h>


EspNowHubClass EspNowHub;


static EspNowHubClass* s_instance = nullptr;


static const uint8_t BROADCAST_MAC[6] =
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };


// The phase vocabulary, matching NodeDiscoveryPhase on the Control Server.
// Transmitted by NAME rather than by ordinal so inserting a phase later cannot
// silently re-point an existing one.
static const char* PHASE_DISCOVERING         = "Discovering";
static const char* PHASE_NODE_FOUND          = "NodeFound";
static const char* PHASE_IDENTITY_CONFIRMED  = "IdentityConfirmed";
static const char* PHASE_REGISTERING         = "Registering";
static const char* PHASE_COMPLETED           = "Completed";
static const char* PHASE_DISCOVERY_FAILED    = "DiscoveryFailed";
static const char* PHASE_REGISTRATION_FAILED = "RegistrationFailed";
static const char* PHASE_CANCELLED           = "Cancelled";


// --------------------------------------------------
// Send callback
// --------------------------------------------------
//
// The last link in the chain: esp_now_send() returning ESP_OK means the frame
// was ACCEPTED for transmission, not that it left the antenna. This says it
// left.
//
// The signature changed in ESP-IDF 5.5 / Arduino core 3.3, which is what this
// project builds against - verified against the working ESP-NOW sketches in
// F:\IOT\ESP32\ESPNOW, which use exactly this form. `info` is deliberately
// NOT dereferenced: its layout differs across core versions and the status is
// the whole point.
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
static void HubSent(
    const wifi_tx_info_t* info,
    esp_now_send_status_t status)
{
#else
static void HubSent(
    const uint8_t* mac,
    esp_now_send_status_t status)
{
#endif

    if (s_instance == nullptr) return;

    s_instance->OnFrameSent(status == ESP_NOW_SEND_SUCCESS);
}


#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
static void HubReceive(
    const esp_now_recv_info_t* info,
    const uint8_t* data,
    int length)
{
    const uint8_t* mac = info->src_addr;
#else
static void HubReceive(
    const uint8_t* mac,
    const uint8_t* data,
    int length)
{
#endif

    if (s_instance == nullptr) return;

    s_instance->OnFrameReceived(mac, data, length);
}


// ==================================================
// Setup
// ==================================================

int EspNowHubClass::declarePersistedNodes()
{
    s_instance = this;

    // --------------------------------------------------
    // The address a node must aim at
    // --------------------------------------------------
    //
    // ESP_MAC_WIFI_STA, not ESP_MAC_BASE. This MAC is handed to the node in
    // the DISCOVERY_REQUEST and becomes the 802.11 DESTINATION address of the
    // node's unicast DISCOVERY_RESPONSE - and a unicast frame whose
    // destination does not match the receiving interface's own address is
    // discarded by the hardware, before ESP-NOW, before any callback, before
    // anything the firmware could log. The hub would hear literally nothing
    // while every other stage looked correct.
    //
    // On these parts the station address is the base address, so this is
    // usually the same six bytes. "Usually" is the problem: the identity MAC
    // and the radio's receive address are different questions, and only one of
    // them is being asked here. StartRadio() logs both so the answer is in the
    // log rather than in this comment.
    esp_read_mac(_hubMac, ESP_MAC_WIFI_STA);


    if (!LoadRegistry())
    {
        Serial.println(
            "[EspNowHub] No node registry. This hub has adopted nothing yet.");

        return 0;
    }


    int declared = 0;

    for (int i = 0; i < _nodeCount; i++)
    {
        String composed =
            ComposeDeviceKey(
                _nodes[i].deviceKey,
                _nodes[i].mac);


        HomeShield.addDevice(
            composed,
            _nodes[i].deviceType,
            _nodes[i].defaultName);


        Serial.print(
            "[EspNowHub] Declared adopted node ");

        Serial.print(_nodes[i].mac);

        Serial.print(" as '");

        Serial.print(composed);

        Serial.println("'");


        declared++;
    }


    return declared;
}


void EspNowHubClass::begin()
{
    s_instance = this;

    // Harmless when declarePersistedNodes() already did it, and necessary when
    // a sketch calls begin() without it.
    esp_read_mac(_hubMac, ESP_MAC_BASE);


    Serial.print(
        "[EspNowHub] Hub MAC: ");

    Serial.println(
        HsEspNow::FormatMac(_hubMac));


    // The radio is not brought up here. An unprovisioned board is advertising
    // over BLE with Wi-Fi down (milestone 37) and has no settled channel; loop()
    // starts ESP-NOW once Wi-Fi is actually up.
}


void EspNowHubClass::StartRadio()
{
    if (_radioStarted) return;


    esp_err_t initResult = esp_now_init();

    // ESP_ERR_ESPNOW_EXIST means it is already up - a success for our purposes,
    // and treating it as a failure would leave the hub retrying forever against
    // a radio that was working the whole time.
    if (initResult != ESP_OK &&
        initResult != ESP_ERR_ESPNOW_EXIST)
    {
        Serial.print(
            "[EspNowHub] esp_now_init() FAILED: ");

        Serial.print(esp_err_to_name(initResult));

        Serial.print(" (Wi-Fi status ");

        Serial.print(WiFi.status());

        Serial.println("). Retrying.");

        return;
    }


    esp_err_t cbResult =
        esp_now_register_recv_cb(HubReceive);

    esp_err_t sendCbResult =
        esp_now_register_send_cb(HubSent);


    // --------------------------------------------------
    // Power save OFF
    // --------------------------------------------------
    //
    // Bring-up fix, and the one that breaks the RESPONSE leg rather than the
    // request leg.
    //
    // This hub is associated with the household router, so the default
    // WIFI_PS_MIN_MODEM puts its radio to sleep between DTIM beacons. Its own
    // broadcasts still go out - transmission is never affected - but a node's
    // DISCOVERY_RESPONSE arrives whenever the node decides to send it, and a
    // sleeping receiver simply misses it. Discovery would then fail at the
    // exact point where everything looked right from the hub's side.
    //
    // The cost is power, on a mains-powered board. That is the correct trade.
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);


    // --------------------------------------------------
    // The broadcast peer
    // --------------------------------------------------
    //
    // channel 0 means "whatever the interface is on", which is the only correct
    // value here: the hub is joined to the router and its channel is the
    // router's. Naming a channel would pin ESP-NOW to one the station interface
    // may have moved off.
    esp_now_peer_info_t peer = {};

    memcpy(peer.peer_addr, BROADCAST_MAC, 6);

    peer.channel = 0;

    peer.encrypt = false;

    peer.ifidx = WIFI_IF_STA;


    esp_err_t peerResult = esp_now_add_peer(&peer);

    if (peerResult != ESP_OK &&
        peerResult != ESP_ERR_ESPNOW_EXIST)
    {
        Serial.print(
            "[EspNowHub] Could not add the broadcast peer: ");

        Serial.println(esp_err_to_name(peerResult));

        return;
    }


    _radioStarted = true;


    uint8_t primary = 0;

    wifi_second_chan_t second;

    esp_wifi_get_channel(&primary, &second);


    Serial.print(
        "[EspNowHub] ESP-NOW ready. init=");

    Serial.print(esp_err_to_name(initResult));

    Serial.print(" recv_cb=");

    Serial.print(esp_err_to_name(cbResult));

    Serial.print(" send_cb=");

    Serial.print(esp_err_to_name(sendCbResult));

    Serial.print(" powerSave=");

    Serial.print(esp_err_to_name(ps));

    Serial.print(" wifiChannel=");

    Serial.print(WiFi.channel());

    Serial.print(" radioChannel=");

    Serial.print(primary);

    Serial.print(" hubMac=");

    Serial.println(HsEspNow::FormatMac(_hubMac));


    // If these three ever disagree, the node has been aiming at an address
    // this radio does not answer to, and that alone explains a hub that hears
    // nothing.
    uint8_t baseMac[6] = {};

    esp_read_mac(baseMac, ESP_MAC_BASE);

    Serial.print(
        "[EspNowHub]   MAC check: espnow/sta=");

    Serial.print(HsEspNow::FormatMac(_hubMac));

    Serial.print(" base=");

    Serial.print(HsEspNow::FormatMac(baseMac));

    Serial.print(" WiFi.macAddress()=");

    Serial.println(WiFi.macAddress());


    Serial.println(
        "[EspNowHub] A node must sweep onto this channel to hear us. If the "
        "node never reports framesHeard > 0, it is not reaching this channel.");
}


// ==================================================
// Main loop
// ==================================================

void EspNowHubClass::loop()
{
    // Said once, the first time this runs with the radio up. It answers the
    // one question the logs could not: whether the sketch is calling
    // EspNowHub.loop() at all after HomeShield.loop(). Without it, a missing
    // call and a discovery that never starts look identical.
    if (_radioStarted && !_loopConfirmed)
    {
        _loopConfirmed = true;

        Serial.println(
            "[EspNowHub] loop() is running and the radio is up. Ready for "
            "DISCOVER_NODE.");
    }


    // ESP-NOW rides the station interface, so it waits for the same Wi-Fi the
    // rest of the module needs. Nothing is lost by the wait: a discovery can
    // only be asked for over MQTT, which needs Wi-Fi too.
    if (!_radioStarted)
    {
        // Rate-limited. StartRadio() now reports its failures, and this runs on
        // every pass of the Arduino loop - without the interval a hub whose
        // ESP-NOW could not start would print the same line thousands of times
        // a second and call esp_now_init() just as often.
        if (WiFi.status() == WL_CONNECTED &&
            (_lastRadioAttemptAt == 0 ||
             millis() - _lastRadioAttemptAt >= RadioRetryInterval))
        {
            _lastRadioAttemptAt = millis();

            StartRadio();
        }

        return;
    }


    // --------------------------------------------------
    // Drain the inbox
    // --------------------------------------------------

    if (_hasInbox)
    {
        uint8_t frame[MaxFrame];

        int length;

        portENTER_CRITICAL(&_inboxLock);
        length = _inboxLength;
        memcpy(frame, (const void*)_inbox, length);
        _hasInbox = false;
        portEXIT_CRITICAL(&_inboxLock);


        if (HsEspNow::ValidHeader(
                frame,
                length,
                MSG_DISCOVERY_RESPONSE,
                sizeof(HsDiscoveryResponse)))
        {
            HsDiscoveryResponse response;

            memcpy(&response, frame, sizeof(response));

            HandleResponse(response);
        }
        else if (HsEspNow::ValidHeader(
                     frame,
                     length,
                     MSG_IDENTITY_CONFIRM,
                     sizeof(HsIdentityConfirm)))
        {
            HsIdentityConfirm confirm;

            memcpy(&confirm, frame, sizeof(confirm));

            HandleConfirm(confirm);
        }
        else if (HsEspNow::ValidHeader(
                     frame,
                     length,
                     MSG_RELATIONSHIP_CHECK,
                     sizeof(HsRelationshipCheck)))
        {
            HsRelationshipCheck check;

            memcpy(&check, frame, sizeof(check));

            HandleRelationshipCheck(check);
        }
        else
        {
            // Heard, but not ours, or not the shape we expect. Worth saying
            // exactly once per discovery rather than silently: a hub that
            // hears the node and still fails is a different problem from a hub
            // that hears nothing, and the two must not look alike in the log.
            if (!_reportedForeignFrame)
            {
                _reportedForeignFrame = true;

                Serial.print(
                    "[EspNowHub] <- a frame was heard but rejected by the "
                    "validator (length=");

                Serial.print(length);

                Serial.println("). It is not a HomeShield discovery frame.");
            }
        }
    }


    unsigned long now = millis();


    switch (_state)
    {
        case State::Normal:
            break;


        case State::Discovering:
        {
            // --------------------------------------------------
            // No timeout. Deliberately.
            // --------------------------------------------------
            //
            // The node may not be powered on yet - that is the normal case, and
            // the app tells the household to go and switch it on. A countdown
            // here would expire while somebody was walking to a door.
            //
            // This ends on an answer, on a cancel, or on a restart.
            if (now - _lastBroadcastAt >= BroadcastInterval)
            {
                Broadcast();
            }

            if (now - _lastStatusAt >= StatusInterval)
            {
                _lastStatusAt = now;

                ReportStatus();
            }

            break;
        }


        case State::NodeFound:
        {
            if (now - _lastAckAt < AckInterval) break;


            if (_ackAttempts >= MaxAckAttempts)
            {
                Serial.println(
                    "[EspNowHub] The node never confirmed. Abandoning the "
                    "session; nothing has been saved.");

                EndSession(
                    PHASE_DISCOVERY_FAILED,
                    "NODE_NOT_CONFIRMED");

                break;
            }

            SendAck(true, REASON_NONE);

            break;
        }


        case State::IdentityConfirmed:
        {
            // Nothing to wait for. The node has committed; the hub's half of
            // the transaction starts now.
            BeginRegistration();

            break;
        }


        case State::Registering:
        {
            // Cancellation does not reach here - see CancelDiscovery(). A
            // registration is a transaction with the Control Server and cannot
            // be half-undone, which is also why the app hides Cancel on the
            // registering screen rather than offering a control that lies.
            PollRegistration();

            break;
        }
    }
}


// ==================================================
// Module commands
// ==================================================

bool EspNowHubClass::handleModuleCommand(
    const String& command)
{
    if (command.startsWith("DISCOVER_NODE:"))
    {
        String target =
            command.substring(
                strlen("DISCOVER_NODE:"));

        Serial.print(
            "[EspNowHub] -> discovery requested: ");

        Serial.println(target);

        StartDiscovery(target);

        return true;
    }


    if (command == "CANCEL_NODE_DISCOVERY")
    {
        Serial.println(
            "[EspNowHub] -> cancel requested");

        CancelDiscovery();

        return true;
    }


    Serial.print(
        "[EspNowHub] Ignored an unrecognised module command: ");

    Serial.println(command);

    return false;
}


void EspNowHubClass::StartDiscovery(
    const String& targetMacText)
{
    String target = targetMacText;

    target.trim();

    target.toUpperCase();


    uint8_t target_mac[6];

    if (!HsEspNow::ParseMac(target, target_mac))
    {
        Serial.print(
            "[EspNowHub] Refused a discovery: '");

        Serial.print(target);

        Serial.println(
            "' is not twelve hexadecimal characters.");

        _targetMacText = target;

        Report(
            PHASE_DISCOVERY_FAILED,
            "INVALID_TARGET");

        _targetMacText = "";

        return;
    }


    // Refused BEFORE a session exists. Adopting a node twice would either
    // collide on the device key or create a second Device for one sensor, and
    // the registry is the authority on which nodes this hub already holds.
    if (IsAdopted(target))
    {
        Serial.print(
            "[EspNowHub] Refused a discovery: ");

        Serial.print(target);

        Serial.println(
            " has already been added to this hub.");

        _targetMacText = target;

        Report(
            PHASE_DISCOVERY_FAILED,
            "NODE_ALREADY_ADDED");

        _targetMacText = "";

        return;
    }


    if (_state != State::Normal)
    {
        Serial.print(
            "[EspNowHub] Refused a discovery: one is already running for ");

        Serial.println(_targetMacText);

        String previous = _targetMacText;

        _targetMacText = target;

        Report(
            PHASE_DISCOVERY_FAILED,
            "HUB_BUSY");

        _targetMacText = previous;

        return;
    }


    if (!_radioStarted)
    {
        // This was the silent one. ESP-NOW not being up produced no serial
        // output at all, so a hub that had received DISCOVER_NODE and could do
        // nothing with it looked identical to a hub that had never received it.
        Serial.print(
            "[EspNowHub] Refused a discovery for ");

        Serial.print(target);

        Serial.print(": ESP-NOW is not running (Wi-Fi status ");

        Serial.print(WiFi.status());

        Serial.println("). Nothing can be transmitted.");

        _targetMacText = target;

        Report(
            PHASE_DISCOVERY_FAILED,
            "ESPNOW_UNAVAILABLE");

        _targetMacText = "";

        return;
    }


    if (_nodeCount >= MAX_NODES)
    {
        Serial.print(
            "[EspNowHub] Refused a discovery: this hub already holds the "
            "maximum of ");

        Serial.print(MAX_NODES);

        Serial.println(" nodes.");

        _targetMacText = target;

        Report(
            PHASE_DISCOVERY_FAILED,
            "HUB_REGISTRY_FULL");

        _targetMacText = "";

        return;
    }


    memcpy(_targetMac, target_mac, 6);

    _targetMacText = target;


    // Random and non-zero. It is what lets a frame from a session that has
    // since been cancelled be dropped rather than mistaken for a live one.
    do
    {
        _sessionId = esp_random();
    }
    while (_sessionId == 0);


    _sequence = 0;

    _ackAttempts = 0;

    _lastAckAt = 0;

    _lastBroadcastAt = 0;

    _pending = NodeRecord();

    _pendingComposedKey = "";


    _state = State::Discovering;

    _broadcastsSent = 0;

    _broadcastsFailed = 0;

    _droppedWrongSession = 0;

    _droppedWrongNode = 0;

    _droppedWrongHub = 0;

    _reportedForeignFrame = false;

    _lastStatusAt = millis();


    // --------------------------------------------------
    // The node's peer entry, before the first frame goes out
    // --------------------------------------------------
    //
    // This used to be added inside HandleResponse() - that is, only once a
    // DISCOVERY_RESPONSE had already been received. Everything the hub sends
    // to the node afterwards needs it, so that was enough to complete a
    // discovery, but it left the peer table empty for the entire window in
    // which the node is trying to reach us. The target's MAC is known right
    // here, from the QR code, and its channel is ours, so there is no reason
    // to wait.
    //
    // HandleResponse() still calls AddNodePeer(); it drops and re-adds, which
    // is harmless and keeps that path correct on its own.
    bool nodePeerAdded = AddNodePeer();


    // Re-asserted per discovery, not only at StartRadio(). The station
    // interface reconnects on its own - a router reboot, a roam, a dropped
    // association - and a reconnect restores the driver's default
    // WIFI_PS_MIN_MODEM. A hub that slept between beacons would transmit its
    // broadcasts perfectly and miss every reply, which is precisely the shape
    // of the failure being chased. Idempotent, so it costs nothing to say
    // again at the one moment it matters.
    esp_err_t psResult = esp_wifi_set_ps(WIFI_PS_NONE);


    uint8_t primary = 0;

    wifi_second_chan_t second;

    esp_wifi_get_channel(&primary, &second);


    Serial.println();

    Serial.println(
        "==================================================");

    Serial.print(
        "[EspNowHub] DISCOVERY STARTED for ");

    Serial.println(_targetMacText);

    Serial.print(
        "[EspNowHub] -> session created : ");

    Serial.println(_sessionId);

    Serial.print(
        "[EspNowHub] -> state = DISCOVERING, channel ");

    Serial.println(primary);

    Serial.print(
        "[EspNowHub]   hub MAC   : ");

    Serial.println(HsEspNow::FormatMac(_hubMac));

    Serial.print(
        "[EspNowHub]   node peer : ");

    Serial.println(
        nodePeerAdded
            ? "added (the hub can hear and address this node)"
            : "NOT ADDED - the node's replies may not be accepted");

    Serial.print(
        "[EspNowHub]   powerSave : ");

    Serial.print(esp_err_to_name(psResult));

    Serial.println(" (WIFI_PS_NONE re-asserted for this session)");

    Serial.print(
        "[EspNowHub]   channel   : ");

    Serial.print(primary);

    Serial.print(" (WiFi.channel()=");

    Serial.print(WiFi.channel());

    Serial.println(")");

    Serial.print(
        "[EspNowHub]   broadcast : every ");

    Serial.print(BroadcastInterval);

    Serial.println(" ms, no timeout");

    Serial.println(
        "==================================================");


    Report(
        PHASE_DISCOVERING,
        nullptr);


    Broadcast();
}


void EspNowHubClass::CancelDiscovery()
{
    if (_state == State::Normal)
    {
        // Idempotent. Cancelling nothing is not an error - the app fires this
        // on navigation away, and the session may already have ended.
        Report(
            PHASE_CANCELLED,
            "CANCELLED");

        return;
    }


    if (_state == State::Registering)
    {
        // Ignored, and honestly so. The Control Server has been asked to create
        // a child and that request cannot be recalled; the outcome will be
        // reported as Completed or RegistrationFailed shortly.
        Serial.println(
            "[EspNowHub] Cancel arrived during registration and is ignored. "
            "The registration will be reported when it settles.");

        return;
    }


    Serial.println(
        "[EspNowHub] Discovery cancelled. Nothing was saved.");


    EndSession(
        PHASE_CANCELLED,
        "CANCELLED");
}


// ==================================================
// ESP-NOW
// ==================================================

void EspNowHubClass::Broadcast()
{
    HsDiscoveryRequest request = {};

    request.header.magic = HS_ESPNOW_MAGIC;
    request.header.protocolVersion = HS_ESPNOW_VERSION;
    request.header.msgType = MSG_DISCOVERY_REQUEST;
    request.header.sequence = _sequence++;
    request.header.sessionId = _sessionId;

    memcpy(request.hubMac, _hubMac, 6);
    memcpy(request.targetMac, _targetMac, 6);


    esp_err_t result =
        esp_now_send(
            BROADCAST_MAC,
            (const uint8_t*)&request,
            sizeof(request));


    _lastBroadcastAt = millis();

    _broadcastsSent++;

    if (result != ESP_OK) _broadcastsFailed++;


    // The first few in full, then one line every five seconds from loop().
    // A send that is failing every time - a missing peer, a radio that is not
    // up - was previously invisible, because nothing checked this return value.
    if (_broadcastsSent <= 3 || result != ESP_OK)
    {
        Serial.print(
            "[EspNowHub] -> DISCOVERY_REQUEST #");

        Serial.print(_broadcastsSent);

        Serial.print(" generated target=");

        Serial.print(HsEspNow::FormatMac(request.targetMac));

        Serial.print(" session=");

        Serial.print(request.header.sessionId);

        Serial.print(" ch=");

        Serial.println(WiFi.channel());

        Serial.print(
            "[EspNowHub] -> esp_now_send() = ");

        Serial.println(esp_err_to_name(result));
    }
}


// --------------------------------------------------
// Periodic status while a discovery is running
// --------------------------------------------------
//
// One line every five seconds. Paired with the node's own status line, the two
// together say exactly where discovery is failing:
//
//   hub sent > 0, node framesHeard == 0   the node never reaches this channel,
//                                          or its radio is asleep
//   hub sent > 0, node framesHeard > 0,
//     node forOtherNodes > 0               wrong target MAC
//   node responded, hub framesHeard == 0   the hub cannot hear the node -
//                                          power save, or out of range
void EspNowHubClass::ReportStatus()
{
    uint8_t primary = 0;

    wifi_second_chan_t second;

    esp_wifi_get_channel(&primary, &second);


    Serial.print("[EspNowHub] discovering ");

    Serial.print(_targetMacText);

    Serial.print(" session=");

    Serial.print(_sessionId);

    Serial.print(" ch=");

    Serial.print(primary);

    Serial.print(" sent=");

    Serial.print(_broadcastsSent);

    Serial.print(" failed=");

    Serial.print(_broadcastsFailed);

    Serial.print(" framesHeard=");

    Serial.println(_framesHeard);
}


void EspNowHubClass::OnFrameSent(
    bool ok)
{
    _sendsCompleted++;

    if (!ok) _sendsFailed++;


    // The first few, then only failures. A broadcast has no acknowledgement to
    // wait for, so SUCCESS here means the radio transmitted it - which is
    // exactly the step between "queued" and "on the air" that nothing else
    // reports.
    if (_sendsCompleted <= 3 || !ok)
    {
        Serial.print(
            "[EspNowHub] -> send callback #");

        Serial.print(_sendsCompleted);

        Serial.println(ok ? " = SUCCESS" : " = FAIL");
    }
}


void EspNowHubClass::OnFrameReceived(
    const uint8_t* mac,
    const uint8_t* data,
    int length)
{
    // Before any validation, for the reason the node counts its own: this is
    // the number that says whether the hub can hear anything at all.
    _framesHeard++;


    // The first few in full. framesHeard rising while discovery still fails
    // would otherwise be just as blind as framesHeard staying at zero: these
    // lines say whether what arrived is a HomeShield frame, whose it is, and
    // why the validator might be turning it away. Same task context as the
    // send callback, which already prints.
    if (_framesHeard <= 5)
    {
        Serial.print(
            "[EspNowHub] <- frame #");

        Serial.print(_framesHeard);

        Serial.print(" from ");

        Serial.print(HsEspNow::FormatMac(mac));

        Serial.print(" length=");

        Serial.print(length);

        if (length >= (int)sizeof(HsEspNowHeader))
        {
            HsEspNowHeader header;

            memcpy(&header, data, sizeof(header));

            Serial.print(" magic=0x");

            Serial.print(header.magic, HEX);

            Serial.print(" version=");

            Serial.print(header.protocolVersion);

            Serial.print(" type=");

            Serial.print(header.msgType);

            Serial.print(" session=");

            Serial.print(header.sessionId);
        }

        Serial.println();
    }

    if (length <= 0 || (size_t)length > MaxFrame) return;


    portENTER_CRITICAL_ISR(&_inboxLock);

    memcpy((void*)_inbox, data, length);

    _inboxLength = length;

    _hasInbox = true;

    portEXIT_CRITICAL_ISR(&_inboxLock);
}


void EspNowHubClass::HandleResponse(
    const HsDiscoveryResponse& response)
{
    if (_state != State::Discovering &&
        _state != State::NodeFound)
    {
        return;
    }


    // --------------------------------------------------
    // Four checks, and every one of them drops silently
    // --------------------------------------------------
    //
    // The node MAC check is the one that matters most: it is what stops a
    // node other than the scanned one being adopted, whether it is
    // misbehaving, misconfigured or hostile. The user scanned a label and
    // that is the only sensor this session can produce.
    // Each drop says so ONCE per session. Silent drops were right for the
    // radio - the hub must not narrate every stray frame - but during bring-up
    // "the node says it replied and the hub shows nothing" had no explanation
    // at all, and these three checks are where such a reply goes to die.
    if (response.header.sessionId != _sessionId)
    {
        if (_droppedWrongSession++ == 0)
        {
            Serial.print(
                "[EspNowHub] Dropped a DISCOVERY_RESPONSE from a stale session: "
                "frame=");

            Serial.print(response.header.sessionId);

            Serial.print(" live=");

            Serial.println(_sessionId);
        }

        return;
    }

    if (memcmp(response.nodeMac, _targetMac, 6) != 0)
    {
        if (_droppedWrongNode++ == 0)
        {
            Serial.print(
                "[EspNowHub] Dropped a DISCOVERY_RESPONSE from ");

            Serial.print(HsEspNow::FormatMac(response.nodeMac));

            Serial.print(", which is not the node being looked for (");

            Serial.print(_targetMacText);

            Serial.println(").");
        }

        return;
    }

    if (memcmp(response.hubMac, _hubMac, 6) != 0)
    {
        if (_droppedWrongHub++ == 0)
        {
            Serial.print(
                "[EspNowHub] Dropped a DISCOVERY_RESPONSE addressed to hub ");

            Serial.print(HsEspNow::FormatMac(response.hubMac));

            Serial.print(", not to this one (");

            Serial.print(HsEspNow::FormatMac(_hubMac));

            Serial.println(").");
        }

        return;
    }


    if (_state == State::NodeFound)
    {
        // A retry of a response we have already accepted. Re-acknowledged so
        // the node stops repeating it, but NOT re-processed: the state machine
        // advances once per session.
        SendAck(true, REASON_NONE);

        return;
    }


    String deviceType;
    String deviceKey;
    String defaultName;


    bool decoded =
        HsEspNow::ReadField(
            response.deviceType,
            sizeof(response.deviceType),
            deviceType) &&
        HsEspNow::ReadField(
            response.deviceKey,
            sizeof(response.deviceKey),
            deviceKey) &&
        HsEspNow::ReadField(
            response.defaultName,
            sizeof(response.defaultName),
            defaultName);


    if (!decoded)
    {
        Serial.println(
            "[EspNowHub] The node's metadata could not be read. Refusing it.");

        SendAck(false, REASON_MALFORMED);

        EndSession(
            PHASE_DISCOVERY_FAILED,
            "NODE_METADATA_INVALID");

        return;
    }


    if (!AddNodePeer())
    {
        SendAck(false, REASON_NONE);

        EndSession(
            PHASE_DISCOVERY_FAILED,
            "ESPNOW_UNAVAILABLE");

        return;
    }


    _pending.mac = _targetMacText;
    _pending.deviceType = deviceType;
    _pending.deviceKey = deviceKey;
    _pending.defaultName = defaultName;

    _pendingComposedKey =
        ComposeDeviceKey(deviceKey, _targetMacText);


    _state = State::NodeFound;

    _ackAttempts = 0;

    _lastAckAt = 0;


    Serial.print(
        "[EspNowHub] Node found: ");

    Serial.print(_pending.mac);

    Serial.print(" type=");

    Serial.print(_pending.deviceType);

    Serial.print(" key=");

    Serial.print(_pending.deviceKey);

    Serial.print(" name=");

    Serial.println(_pending.defaultName);


    Report(
        PHASE_NODE_FOUND,
        nullptr);


    SendAck(true, REASON_NONE);
}


void EspNowHubClass::HandleConfirm(
    const HsIdentityConfirm& confirm)
{
    if (_state != State::NodeFound) return;

    if (confirm.header.sessionId != _sessionId) return;

    if (memcmp(confirm.nodeMac, _targetMac, 6) != 0) return;

    if (memcmp(confirm.hubMac, _hubMac, 6) != 0) return;


    if (confirm.stored == 0)
    {
        // The node could not write our MAC, so it is NOT adopted and must not
        // become a child Device. It reports this rather than going quiet
        // precisely so the household is told instead of left watching a radar.
        Serial.println(
            "[EspNowHub] The node could not persist this hub's address. "
            "Abandoning.");

        EndSession(
            PHASE_DISCOVERY_FAILED,
            "NODE_STORAGE_FAILED");

        return;
    }


    _state = State::IdentityConfirmed;


    Serial.println(
        "[EspNowHub] Identity confirmed by the node.");


    Report(
        PHASE_IDENTITY_CONFIRMED,
        nullptr);
}


// ==================================================
// Relationship check (milestone 40)
// ==================================================
//
// A node that booted with this hub's MAC in its NVS is asking whether that is
// still true. The registry IS the answer - the same array declarePersistedNodes()
// re-declares children from - so this needs no network, no Control Server and
// no session.
//
// Answered in ANY state, including mid-discovery. It writes nothing, it does
// not touch _targetMac, _sessionId or _state, and the peer it adds is removed
// again unless the discovery session already owned it. A node waking up must
// not be able to disturb a household standing in front of a different sensor.
//
// The interesting case is the one M40 exists for: the hub was deleted, cleared
// its own NVS and was re-onboarded, so its registry is EMPTY. Every node that
// used to belong to it is then correctly told REVOKED, without the server ever
// having to reach hardware that was asleep at the time.
void EspNowHubClass::HandleRelationshipCheck(
    const HsRelationshipCheck& check)
{
    // Addressed to this hub, and says so. ESP-NOW hands us frames we are the
    // destination of, but the node's stored MAC is the thing being tested and
    // it has to be the thing we compare.
    if (memcmp(check.hubMac, _hubMac, 6) != 0)
    {
        return;
    }


    String nodeMacText =
        HsEspNow::FormatMac(check.nodeMac);


    bool adopted =
        IsAdopted(nodeMacText);


    HsRelationshipStatus status = {};

    status.header.magic = HS_ESPNOW_MAGIC;
    status.header.protocolVersion = HS_ESPNOW_VERSION;
    status.header.msgType = MSG_RELATIONSHIP_STATUS;
    status.header.sequence = 0;

    // The NODE'S session id, echoed. This exchange belongs to the node and the
    // hub's own _sessionId - which may be mid-discovery for somebody else -
    // has nothing to do with it.
    status.header.sessionId = check.header.sessionId;

    memcpy(status.hubMac, _hubMac, 6);
    memcpy(status.nodeMac, check.nodeMac, 6);

    status.verdict =
        adopted ? RELATIONSHIP_VALID : RELATIONSHIP_REVOKED;


    // --------------------------------------------------
    // The reply peer
    // --------------------------------------------------
    //
    // Unicast needs a peer entry. If one already exists - because a discovery
    // session is talking to this very node - it belongs to that session and is
    // left completely alone.
    //
    // Otherwise ONE reusable slot is kept, and the previous occupant is
    // evicted when the next node asks. It is deliberately not deleted straight
    // after the send: esp_now_send() only QUEUES the frame, and removing the
    // peer underneath it is how a reply gets dropped between "sent" and
    // actually transmitted. Evicting on the NEXT check gives the current one
    // all the time it needs, and one stale entry costs nothing against
    // ESP-NOW's peer table.
    if (!esp_now_is_peer_exist(check.nodeMac))
    {
        if (_replyPeerAdded &&
            memcmp(_replyPeer, check.nodeMac, 6) != 0)
        {
            esp_now_del_peer(_replyPeer);

            _replyPeerAdded = false;
        }


        esp_now_peer_info_t peer = {};

        memcpy(peer.peer_addr, check.nodeMac, 6);

        // The interface's channel. The node is sweeping and sent this from the
        // channel this hub is pinned to, which is the one it is listening on
        // right now.
        peer.channel = 0;

        peer.encrypt = false;

        peer.ifidx = WIFI_IF_STA;

        esp_err_t addResult = esp_now_add_peer(&peer);

        if (addResult != ESP_OK &&
            addResult != ESP_ERR_ESPNOW_EXIST)
        {
            Serial.print(
                "[EspNowHub] Could not add ");

            Serial.print(nodeMacText);

            Serial.print(" as a peer to answer its relationship check: ");

            Serial.println(esp_err_to_name(addResult));

            return;
        }


        memcpy(_replyPeer, check.nodeMac, 6);

        _replyPeerAdded = true;
    }


    esp_err_t sendResult =
        esp_now_send(
            check.nodeMac,
            (const uint8_t*)&status,
            sizeof(status));


    Serial.print(
        "[EspNowHub] Relationship check from ");

    Serial.print(nodeMacText);

    Serial.print(" -> ");

    Serial.print(adopted ? "VALID" : "REVOKED");

    Serial.print(" (registry holds ");

    Serial.print(_nodeCount);

    Serial.print(" node(s)) send=");

    Serial.println(esp_err_to_name(sendResult));
}


void EspNowHubClass::SendAck(
    bool accepted,
    uint8_t reason)
{
    HsDiscoveryAck ack = {};

    ack.header.magic = HS_ESPNOW_MAGIC;
    ack.header.protocolVersion = HS_ESPNOW_VERSION;
    ack.header.msgType = MSG_DISCOVERY_ACK;
    ack.header.sequence = _sequence++;
    ack.header.sessionId = _sessionId;

    memcpy(ack.hubMac, _hubMac, 6);
    memcpy(ack.nodeMac, _targetMac, 6);

    ack.accepted = accepted ? 1 : 0;

    ack.reason = reason;


    esp_now_send(
        _targetMac,
        (const uint8_t*)&ack,
        sizeof(ack));


    _ackAttempts++;

    _lastAckAt = millis();
}


bool EspNowHubClass::AddNodePeer()
{
    DropNodePeer();


    esp_now_peer_info_t peer = {};

    memcpy(peer.peer_addr, _targetMac, 6);

    // The interface's channel, as with the broadcast peer. The node locked onto
    // this channel when it heard us, so both ends agree without either naming a
    // number.
    peer.channel = 0;

    peer.encrypt = false;

    peer.ifidx = WIFI_IF_STA;


    esp_err_t addResult = esp_now_add_peer(&peer);

    if (addResult != ESP_OK &&
        addResult != ESP_ERR_ESPNOW_EXIST)
    {
        Serial.print(
            "[EspNowHub] Could not add the node as a peer: ");

        Serial.println(esp_err_to_name(addResult));

        return false;
    }


    _nodePeerAdded = true;

    return true;
}


void EspNowHubClass::DropNodePeer()
{
    if (!_nodePeerAdded) return;

    esp_now_del_peer(_targetMac);

    _nodePeerAdded = false;
}


// ==================================================
// Registration
// ==================================================

void EspNowHubClass::BeginRegistration()
{
    Serial.print(
        "[EspNowHub] Registering '");

    Serial.print(_pendingComposedKey);

    Serial.println("' with the Control Server.");


    _state = State::Registering;


    Report(
        PHASE_REGISTERING,
        nullptr);


    // Declares the child and asks RegistrationService to re-register the whole
    // module. Refused only for a reason that would also make persisting wrong -
    // a duplicate key, a full table - so a false here ends the session without
    // writing anything.
    if (!HomeShield.addDeviceAtRuntime(
            _pendingComposedKey,
            _pending.deviceType,
            _pending.defaultName))
    {
        EndSession(
            PHASE_REGISTRATION_FAILED,
            "REGISTRATION_REFUSED");
    }
}


void EspNowHubClass::PollRegistration()
{
    auto result =
        HomeShield.reRegistrationState();


    if (result == HomeShieldClass::ReRegistration::InProgress)
    {
        return;
    }


    if (result == HomeShieldClass::ReRegistration::Failed)
    {
        // The node is NOT persisted. The design's ordering is the whole point:
        // registration first, registry second, so a refused declaration leaves
        // the hub exactly as it was and the retry on the phone re-runs
        // registration alone.
        Serial.println(
            "[EspNowHub] Registration failed. The node has NOT been saved.");

        // A 4xx is the Control Server refusing the DECLARATION - an unknown
        // device type, a bad key, a duplicate. A zero or a 5xx is the server
        // not answering. The household is told different things, and retrying
        // helps in only one of the two cases.
        int status =
            HomeShield.lastRegistrationStatus();

        EndSession(
            PHASE_REGISTRATION_FAILED,
            (status >= 400 && status < 500)
                ? "REGISTRATION_REFUSED"
                : "REGISTRATION_UNREACHABLE");

        return;
    }


    // Anything that is not an explicit Succeeded is NOT a success.
    //
    // This used to fall through: the two checks above return on InProgress and
    // on Failed, and everything else was taken to mean the registration had
    // landed. The enum has a third value - Idle, which is what
    // reRegistrationState() answers when there is no registration service at
    // all - and reading it as success would report Completed for a child the
    // Control Server was never asked to create, leaving the household with a
    // sensor that exists only on this hub.
    if (result != HomeShieldClass::ReRegistration::Succeeded)
    {
        Serial.println(
            "[EspNowHub] Registration ended in an unexpected state. The node has "
            "NOT been saved.");

        EndSession(
            PHASE_REGISTRATION_FAILED,
            "REGISTRATION_UNREACHABLE");

        return;
    }


    // Succeeded. Only now does the node become permanent.
    if (!AppendToRegistry(_pending))
    {
        // The child Device exists and the hub cannot remember it. Reported
        // honestly rather than as a success: on the next boot this hub will not
        // declare that child and the Control Server will disable it.
        Serial.println(
            "[EspNowHub] The node registered but could not be written to NVS.");

        EndSession(
            PHASE_REGISTRATION_FAILED,
            "HUB_STORAGE_FAILED");

        return;
    }


    Serial.print(
        "[EspNowHub] Node added. This hub now holds ");

    Serial.print(_nodeCount);

    Serial.println(" node(s).");


    EndSession(
        PHASE_COMPLETED,
        nullptr);
}


// ==================================================
// Session lifecycle
// ==================================================

void EspNowHubClass::EndSession(
    const char* phase,
    const char* failureCode)
{
    Report(phase, failureCode);


    DropNodePeer();


    memset(_targetMac, 0, sizeof(_targetMac));

    _targetMacText = "";

    _sessionId = 0;

    _sequence = 0;

    _ackAttempts = 0;

    _lastAckAt = 0;

    _lastBroadcastAt = 0;

    _pending = NodeRecord();

    _pendingComposedKey = "";

    _state = State::Normal;
}


void EspNowHubClass::Report(
    const char* phase,
    const char* failureCode)
{
    String payload =
        "{\"phase\":\"" +
        String(phase) +
        "\",\"nodeMac\":\"" +
        JsonLite::Escape(_targetMacText) +
        "\"";


    // The node's own metadata travels from NodeFound onwards, because that is
    // when the hub learns it and because the app shows it on the Device Found
    // screen as evidence of what the sensor said about itself.
    if (_pending.mac.length() > 0)
    {
        payload +=
            ",\"deviceType\":\"" +
            JsonLite::Escape(_pending.deviceType) +
            "\",\"deviceKey\":\"" +
            JsonLite::Escape(_pending.deviceKey) +
            "\",\"defaultName\":\"" +
            JsonLite::Escape(_pending.defaultName) +
            "\"";
    }


    if (failureCode != nullptr)
    {
        payload +=
            ",\"failureCode\":\"" +
            String(failureCode) +
            "\"";
    }


    payload += "}";


    // Module-scoped: no DeviceKey, because there is no child device yet -
    // creating one is what this sequence is trying to do.
    if (!HomeShield.publishModuleEvent(
            DeviceEventTypes::NodeDiscovery,
            payload))
    {
        // MQTT is down. Said plainly rather than retried: the Control Server
        // holds the session in memory and will report the hub unreachable, and
        // a queue of stale phases arriving after the user gave up would be
        // worse than silence.
        Serial.print(
            "[EspNowHub] Could not publish the discovery phase '");

        Serial.print(phase);

        Serial.println("' - MQTT is not connected.");
    }
}


// ==================================================
// Registry
// ==================================================

String EspNowHubClass::ComposeDeviceKey(
    const String& deviceKey,
    const String& mac)
{
    String composed = deviceKey;

    composed.toLowerCase();

    String lowerMac = mac;

    lowerMac.toLowerCase();


    // "door-70af0935923c" - 17 characters, inside the Control Server's 32, and
    // matching its [a-z0-9_-] rule. Unique by construction, and derived only
    // from values the registry holds, so it is identical on every boot and every
    // child keeps its Device.Id.
    return composed + "-" + lowerMac;
}


bool EspNowHubClass::IsAdopted(
    const String& mac) const
{
    for (int i = 0; i < _nodeCount; i++)
    {
        if (_nodes[i].mac == mac) return true;
    }

    return false;
}


int EspNowHubClass::NodeCount() const
{
    return _nodeCount;
}


String EspNowHubClass::EncodeRecord(
    const NodeRecord& record)
{
    // Pipe-separated. None of the four fields can contain a pipe: a MAC is hex,
    // a device type and a device key are constrained character sets, and a
    // default name comes from a compiled-in constant in the node's own sketch.
    return
        record.mac + "|" +
        record.deviceType + "|" +
        record.deviceKey + "|" +
        record.defaultName;
}


bool EspNowHubClass::DecodeRecord(
    const String& text,
    NodeRecord& record)
{
    int first = text.indexOf('|');

    if (first < 0) return false;

    int second = text.indexOf('|', first + 1);

    if (second < 0) return false;

    int third = text.indexOf('|', second + 1);

    if (third < 0) return false;


    record.mac = text.substring(0, first);
    record.deviceType = text.substring(first + 1, second);
    record.deviceKey = text.substring(second + 1, third);
    record.defaultName = text.substring(third + 1);


    return
        record.mac.length() == 12 &&
        record.deviceType.length() > 0 &&
        record.deviceKey.length() > 0;
}


bool EspNowHubClass::LoadRegistry()
{
    if (!_preferences.begin(Namespace, true)) return false;


    int state =
        _preferences.getInt(StateKey, 0);

    int count =
        _preferences.getInt(CountKey, 0);


    if (state != 1 || count <= 0)
    {
        _preferences.end();

        return false;
    }


    if (count > MAX_NODES) count = MAX_NODES;


    _nodeCount = 0;

    for (int i = 0; i < count; i++)
    {
        String key = "n" + String(i);

        String text =
            _preferences.getString(key.c_str(), "");


        NodeRecord record;

        if (!DecodeRecord(text, record))
        {
            // Skipped rather than fatal. One unreadable record must not cost
            // the household every other sensor on the hub.
            Serial.print(
                "[EspNowHub] Registry record ");

            Serial.print(i);

            Serial.println(" is unreadable and was skipped.");

            continue;
        }


        _nodes[_nodeCount] = record;

        _nodeCount++;
    }


    _preferences.end();


    Serial.print(
        "[EspNowHub] Loaded ");

    Serial.print(_nodeCount);

    Serial.println(" adopted node(s) from NVS.");


    return _nodeCount > 0;
}


bool EspNowHubClass::AppendToRegistry(
    const NodeRecord& record)
{
    if (_nodeCount >= MAX_NODES) return false;

    // Re-checked here as well as before the session. The two checks are a
    // second apart in wall-clock time and minutes apart in the flow.
    if (IsAdopted(record.mac)) return false;


    if (!_preferences.begin(Namespace, false)) return false;


    String key = "n" + String(_nodeCount);

    String text = EncodeRecord(record);


    bool ok =
        _preferences.putString(key.c_str(), text) > 0;

    // Verified before anything points at it.
    ok = ok &&
        _preferences.getString(key.c_str(), "") == text;

    ok = ok &&
        _preferences.putInt(VersionKey, CurrentVersion) > 0;


    // The count SECOND: a record nothing references is harmless, a count
    // pointing at a record that was never written is not.
    if (ok)
    {
        ok = _preferences.putInt(CountKey, _nodeCount + 1) > 0;
    }


    // The marker LAST.
    if (ok)
    {
        ok = _preferences.putInt(StateKey, 1) > 0;
    }


    _preferences.end();


    if (!ok) return false;


    _nodes[_nodeCount] = record;

    _nodeCount++;

    return true;
}
