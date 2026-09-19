#include "EspNowNode.h"
#include "Debug.h"
#include "DeviceIdentity.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_random.h>


EspNowNodeClass EspNowNode;


static EspNowNodeClass* s_instance = nullptr;


#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
static void NodeReceive(
    const esp_now_recv_info_t* info,
    const uint8_t* data,
    int length)
{
    const uint8_t* mac = info->src_addr;
#else
static void NodeReceive(
    const uint8_t* mac,
    const uint8_t* data,
    int length)
{
#endif

    if (s_instance == nullptr) return;

    s_instance->OnFrameReceived(mac, data, length);
}


// --------------------------------------------------
// The send callback
// --------------------------------------------------
//
// The one number this node was missing, and the reason discovery could look
// perfect from here while the hub heard nothing at all.
//
// esp_now_send() returning ESP_OK means the frame was ACCEPTED into the
// transmit queue. It says nothing about whether it was transmitted, and
// nothing about whether anybody heard it. This callback says both, because a
// DISCOVERY_RESPONSE is UNICAST: the 802.11 layer waits for an acknowledgement
// from the hub's radio and reports SUCCESS only when one arrives.
//
// That makes this the dividing line of the whole investigation:
//
//   send callback SUCCESS   the hub's RADIO received the frame and
//                           acknowledged it. Anything still missing after that
//                           is above the radio - the hub's ESP-NOW layer or
//                           its own software.
//
//   send callback FAIL      no acknowledgement. The frame never reached the
//                           hub: wrong destination MAC, wrong channel, the
//                           hub's receiver asleep, or out of range.
//
// Signature as in EspNowHub.cpp, for the same reason, and `info` is likewise
// not dereferenced.
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
static void NodeSent(
    const wifi_tx_info_t* info,
    esp_now_send_status_t status)
{
#else
static void NodeSent(
    const uint8_t* mac,
    esp_now_send_status_t status)
{
#endif

    if (s_instance == nullptr) return;

    s_instance->OnFrameSent(status == ESP_NOW_SEND_SUCCESS);
}


// ==================================================
// Starting up
// ==================================================

void EspNowNodeClass::begin(
    const String& deviceType,
    const String& deviceKey,
    const String& defaultName)
{
    s_instance = this;

    _deviceType = deviceType;
    _deviceKey = deviceKey;
    _defaultName = defaultName;


    // eFuse, before anything else. It is this node's whole identity and it is
    // what the QR code on the enclosure carries.
    esp_read_mac(_nodeMac, ESP_MAC_BASE);


    Serial.print(
        "[EspNowNode] Node MAC: ");

    Serial.println(
        HsEspNow::FormatMac(_nodeMac));


    if (Load())
    {
        _persisted = true;

        Serial.print(
            "[EspNowNode] Provisioned. Hub: ");

        Serial.println(
            _hubMacText);


        // --------------------------------------------------
        // Milestone 40: ask before settling
        // --------------------------------------------------
        //
        // M38 stopped here with the radio down, because a provisioned node had
        // nothing to say. It has exactly one thing to say now, once, at boot:
        // is the hub I stored still mine?
        //
        // The server never commands a C3. A node may be asleep or unpowered
        // for days when its hub is deleted, so there is nothing to deliver a
        // revocation TO - the node has to come and ask. This is that ask, and
        // it is the whole of it: one short sweep, then silence either way.
        //
        // It cannot adopt this node to anybody and it cannot change which hub
        // it belongs to. The only outcome that writes to NVS is an explicit
        // REVOKED from the hub whose MAC is already stored here.
        _state = State::RelationshipCheck;

        _sweepsCompleted = 0;

        do
        {
            _sessionId = esp_random();
        }
        while (_sessionId == 0);


        Serial.println(
            "[EspNowNode] Verifying the stored hub relationship before "
            "settling.");


        StartRadio();

        // Channel 1 is already set and dwelling; the sweep in loop() sends on
        // every channel AFTER it steps, so the first one is sent here.
        SendRelationshipCheck();

        return;
    }


    Serial.println(
        "[EspNowNode] Unprovisioned. Listening for a hub.");


    StartRadio();
}


void EspNowNodeClass::StartRadio()
{
    // --------------------------------------------------
    // Station mode, and nothing else allowed to touch the radio
    // --------------------------------------------------
    //
    // ESP-NOW needs no association, and staying off any access point is what
    // leaves this node free to change channel. The three calls around the mode
    // change are bring-up fixes, and each one is load-bearing:
    //
    //   persistent(false)      stop the Wi-Fi driver writing credentials into
    //                          its own NVS. This board was an M37-provisioned
    //                          door sensor in a previous life and very likely
    //                          still has an SSID stored there.
    //
    //   setAutoReconnect(false) stop the STA trying to use them. A background
    //                          reconnect SCANS, and a scan hops channels - it
    //                          would fight esp_wifi_set_channel() below and
    //                          leave the node listening on channels nobody is
    //                          broadcasting on, intermittently and invisibly.
    //
    //   set_ps(WIFI_PS_NONE)   stop the radio sleeping. The default is
    //                          WIFI_PS_MIN_MODEM, and a receiver that sleeps
    //                          drops ESP-NOW frames - which is exactly the
    //                          "sweeping, but hears nothing" symptom.
    WiFi.persistent(false);

    WiFi.mode(WIFI_STA);
	
	WiFi.setTxPower(WIFI_POWER_8_5dBm);

    WiFi.setAutoReconnect(false);

    WiFi.disconnect(false, false);


    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);

    Serial.print(
        "[EspNowNode] Power save disabled: ");

    Serial.println(esp_err_to_name(ps));


    _channel = FirstChannel;

    esp_err_t channelResult =
        esp_wifi_set_channel(
            _channel,
            WIFI_SECOND_CHAN_NONE);

    _channelSteppedAt = millis();


    esp_err_t initResult = esp_now_init();

    if (initResult != ESP_OK)
    {
        Serial.print(
            "[EspNowNode] esp_now_init() FAILED: ");

        Serial.println(esp_err_to_name(initResult));

        Serial.println(
            "[EspNowNode] This node cannot be onboarded until it is restarted.");

        return;
    }


    // Receiving a broadcast needs no peer. Sending does, and the hub is added as
    // one only once its MAC is known.
    esp_err_t cbResult =
        esp_now_register_recv_cb(NodeReceive);

    esp_err_t sendCbResult =
        esp_now_register_send_cb(NodeSent);


    _radioStarted =
        cbResult == ESP_OK;


    Serial.print(
        "[EspNowNode] ESP-NOW init=");

    Serial.print(esp_err_to_name(initResult));

    Serial.print(" recv_cb=");

    Serial.print(esp_err_to_name(cbResult));

    Serial.print(" send_cb=");

    Serial.print(esp_err_to_name(sendCbResult));

    Serial.print(" set_channel(");

    Serial.print(_channel);

    Serial.print(")=");

    Serial.println(esp_err_to_name(channelResult));


    Serial.print(
        "[EspNowNode] Sweeping channels ");

    Serial.print(FirstChannel);

    Serial.print("-");

    Serial.print(LastChannel);

    Serial.print(", dwelling ");

    Serial.print(ChannelDwell);

    Serial.println(" ms on each.");
}


// ==================================================
// Main loop
// ==================================================

void EspNowNodeClass::loop()
{
    if (_state == State::Provisioned)
    {
        // The whole of a provisioned node's behaviour in milestone 38.
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
                MSG_DISCOVERY_REQUEST,
                sizeof(HsDiscoveryRequest)))
        {
            HsDiscoveryRequest request;

            memcpy(&request, frame, sizeof(request));

            HandleRequest(request);
        }
        else if (HsEspNow::ValidHeader(
                     frame,
                     length,
                     MSG_DISCOVERY_ACK,
                     sizeof(HsDiscoveryAck)))
        {
            HsDiscoveryAck ack;

            memcpy(&ack, frame, sizeof(ack));

            HandleAck(ack);
        }
        else if (HsEspNow::ValidHeader(
                     frame,
                     length,
                     MSG_RELATIONSHIP_STATUS,
                     sizeof(HsRelationshipStatus)))
        {
            HsRelationshipStatus status;

            memcpy(&status, frame, sizeof(status));

            HandleRelationshipStatus(status);
        }

        // Anything else is dropped in silence. A frame from another HomeShield
        // milestone, another protocol or another vendor is not this node's
        // business and saying so on the serial port every 300 ms would bury
        // everything that is.
    }


    unsigned long now = millis();


    if (now - _lastStatusAt >= StatusInterval)
    {
        _lastStatusAt = now;

        ReportStatus();
    }


    switch (_state)
    {
        case State::Discovery:
        {
            // Nothing has named this node yet. Keep sweeping.
            if (now - _channelSteppedAt >= ChannelDwell)
            {
                StepChannel();
            }

            break;
        }

        case State::Responding:
        {
            if (now - _lastResponseAt < ResponseInterval) break;


            if (_responseAttempts >= MaxResponseAttempts)
            {
                // The hub heard us or it did not; either way this session is
                // over and nothing was written. Back to sweeping - if the hub
                // is still discovering, its next broadcast starts a fresh one.
                Serial.println(
                    "[EspNowNode] No acknowledgement after 5 attempts. "
                    "Returning to discovery.");

                AbandonSession();

                break;
            }

            SendResponse();

            break;
        }

        case State::Confirmed:
        {
            if (now - _confirmedAt < RestartDelay) break;

            Serial.println(
                "[EspNowNode] Onboarded. Restarting.");

            Serial.flush();

            ESP.restart();

            break;
        }

        case State::RelationshipCheck:
        {
            // --------------------------------------------------
            // Milestone 40
            // --------------------------------------------------
            //
            // The hub cannot change channel - it is pinned to the household
            // router - and this node has no way to know which channel that is,
            // exactly as during discovery. So it sweeps, and asks once per
            // channel visit.
            //
            // Two full sweeps, then it stops. Giving up is a NORMAL outcome
            // here, not a failure: the hub may be powered off, out of range or
            // rebooting, and none of that means this node was revoked.
            if (_sweepsCompleted >= MaxCheckSweeps)
            {
                SettleProvisioned(
                    "the hub did not answer within two sweeps");

                break;
            }


            if (now - _channelSteppedAt >= ChannelDwell)
            {
                StepChannel();

                SendRelationshipCheck();
            }

            break;
        }

        case State::Provisioned:
            break;
    }
}


void EspNowNodeClass::StepChannel()
{
    _channel++;

    if (_channel > LastChannel)
    {
        _channel = FirstChannel;

        _sweepsCompleted++;
    }


    // The return value matters. A failing set_channel is invisible otherwise,
    // and a node pinned to one channel while believing it is sweeping is the
    // hardest version of this fault to see.
    esp_err_t result =
        esp_wifi_set_channel(
            _channel,
            WIFI_SECOND_CHAN_NONE);

    if (result != ESP_OK)
    {
        Serial.print(
            "[EspNowNode] set_channel(");

        Serial.print(_channel);

        Serial.print(") FAILED: ");

        Serial.println(esp_err_to_name(result));
    }

    _channelSteppedAt = millis();
}


// --------------------------------------------------
// Periodic status
// --------------------------------------------------
//
// One line every five seconds while unprovisioned. It exists because
// "Sweeping channels 1-13" was printed once and then the node went silent
// forever, whether it was working perfectly or deaf - and those two look
// identical from the outside.
//
// framesHeard is the decisive number. Zero means nothing is reaching this
// radio: wrong channel, power save, or the hub is not transmitting. Non-zero
// with forOthers rising means the hub IS heard and is asking for a different
// node.
void EspNowNodeClass::ReportStatus()
{
    uint8_t primary = 0;

    wifi_second_chan_t second;

    esp_wifi_get_channel(&primary, &second);


    if (!_radioStarted)
    {
        Serial.println(
            "[EspNowNode] RADIO NOT STARTED - ESP-NOW failed to initialise. "
            "This node cannot be discovered until it is restarted.");

        return;
    }


    Serial.print("[EspNowNode] listening ch=");

    Serial.print(_channel);

    Serial.print(" (radio ch=");

    Serial.print(primary);

    Serial.print(") sweeps=");

    Serial.print(_sweepsCompleted);

    Serial.print(" framesHeard=");

    Serial.print(_framesHeard);

    Serial.print(" forOtherNodes=");

    Serial.print(_framesForOthers);

    Serial.print(" sent=");

    Serial.print(_sendsCompleted);

    Serial.print(" acked=");

    Serial.print(_sendsCompleted - _sendsFailed);

    Serial.print(" state=");

    switch (_state)
    {
        case State::Discovery:    Serial.println("DISCOVERY"); break;
        case State::Responding:   Serial.println("RESPONDING"); break;
        case State::Confirmed:    Serial.println("CONFIRMED"); break;
        case State::RelationshipCheck:
                                  Serial.println("RELATIONSHIP_CHECK"); break;
        case State::Provisioned:  Serial.println("PROVISIONED"); break;
    }
}


// ==================================================
// Outbound
// ==================================================

void EspNowNodeClass::OnFrameSent(
    bool ok)
{
    _sendsCompleted++;

    if (!ok) _sendsFailed++;


    // Every one of them, for now. There are only ever five per discovery
    // cycle, and each is a direct answer to the question the hub's silence
    // raised.
    Serial.print(
        "[EspNowNode] -> send callback #");

    Serial.print(_sendsCompleted);

    if (ok)
    {
        Serial.println(
            " = SUCCESS (the hub's radio acknowledged this frame)");
    }
    else
    {
        Serial.println(
            " = FAIL (no acknowledgement - the frame did not reach the hub)");
    }
}


// ==================================================
// Inbound
// ==================================================

void EspNowNodeClass::OnFrameReceived(
    const uint8_t* mac,
    const uint8_t* data,
    int length)
{
    // Counted BEFORE any validation, and before the length guard rejects
    // anything. This is the number that answers "is this node hearing the hub
    // at all" - the one thing the old logging could not tell anybody.
    _framesHeard++;

    if (length <= 0 || (size_t)length > MaxFrame) return;


    portENTER_CRITICAL_ISR(&_inboxLock);

    // A frame arriving while the previous one is unread is a retry of it. The
    // newest is the one worth keeping.
    memcpy((void*)_inbox, data, length);

    _inboxLength = length;

    _hasInbox = true;

    portEXIT_CRITICAL_ISR(&_inboxLock);
}


void EspNowNodeClass::HandleRequest(
    const HsDiscoveryRequest& request)
{
    // --------------------------------------------------
    // Milestone 40
    // --------------------------------------------------
    //
    // A provisioned node does not answer discovery. M38 enforced that by
    // keeping the radio off entirely; the relationship check now has the radio
    // up for a few seconds at boot, so the rule has to be stated rather than
    // implied.
    //
    // Without this, a hub mid-discovery for this exact MAC could re-adopt a
    // node that already belongs to another hub, during the very window in
    // which it is asking whether it still does.
    if (_persisted)
    {
        return;
    }


    // --------------------------------------------------
    // The check that makes a room full of nodes safe
    // --------------------------------------------------
    //
    // Every unprovisioned node in range hears this broadcast. Exactly one of
    // them is being asked for, and the rest drop it here without a word. There
    // is no "first to answer wins" and no proximity heuristic: the user scanned
    // a specific label and that is the node that responds.
    if (memcmp(request.targetMac, _nodeMac, 6) != 0)
    {
        // Heard, understood, and not for us. Counted rather than printed on
        // every frame - the hub broadcasts twice a second - but the FIRST one
        // is worth saying out loud, because "the hub is audible and is asking
        // for a different MAC" is a completely different fault from silence.
        _framesForOthers++;

        if (_framesForOthers == 1)
        {
            Serial.print(
                "[EspNowNode] Heard a discovery request for ");

            Serial.print(HsEspNow::FormatMac(request.targetMac));

            Serial.print(", which is not this node (");

            Serial.print(HsEspNow::FormatMac(_nodeMac));

            Serial.println("). Ignoring, as designed.");
        }

        return;
    }


    if (_state == State::Responding)
    {
        // The same hub is still asking. Keep the session we have rather than
        // restarting the attempt counter on every 500 ms broadcast, which would
        // make MaxResponseAttempts unreachable.
        if (request.header.sessionId == _sessionId &&
            memcmp(request.hubMac, _hubMac, 6) == 0)
        {
            return;
        }


        // A different session or a different hub. The newer request wins: the
        // old one is either cancelled or from a hub that has given up, and
        // nothing has been written either way.
        Serial.println(
            "[EspNowNode] A new discovery session superseded the current one.");
    }


    memcpy(_hubMac, request.hubMac, 6);

    _hubMacText = HsEspNow::FormatMac(_hubMac);

    _sessionId = request.header.sessionId;

    _sequence = 0;

    _responseAttempts = 0;

    _lastResponseAt = 0;


    // --------------------------------------------------
    // Lock the channel
    // --------------------------------------------------
    //
    // The sweep stops here. The hub cannot move - it is pinned to the router -
    // so the rest of this exchange happens on the channel the request arrived
    // on, and stepping away now would lose the ACK.
    _channelSteppedAt = millis();


    Serial.print(
        "[EspNowNode] Discovery request from hub ");

    Serial.print(_hubMacText);

    Serial.print(" on channel ");

    Serial.println(_channel);


    if (!AddHubPeer())
    {
        Serial.println(
            "[EspNowNode] Could not add the hub as a peer. Returning to "
            "discovery.");

        AbandonSession();

        return;
    }


    _state = State::Responding;


    SendResponse();
}


void EspNowNodeClass::HandleAck(
    const HsDiscoveryAck& ack)
{
    if (_state != State::Responding &&
        _state != State::Confirmed)
    {
        return;
    }


    // Three checks, and all three have to pass. The session id rejects a frame
    // from an exchange that has since been cancelled; the hub MAC rejects an
    // acknowledgement from a hub this node never answered; the node MAC rejects
    // one meant for whichever node the hub spoke to last.
    if (ack.header.sessionId != _sessionId) return;

    if (memcmp(ack.hubMac, _hubMac, 6) != 0) return;

    if (memcmp(ack.nodeMac, _nodeMac, 6) != 0) return;


    // --------------------------------------------------
    // A repeated ACK after this node already committed
    // --------------------------------------------------
    //
    // The hub only retries its acknowledgement when it has not heard the
    // confirmation, so a duplicate arriving here means the confirmation was
    // lost, not that anything is wrong. Re-sending it is what makes the hub's
    // five-attempt budget mean five chances rather than one.
    //
    // Without this the node would sit silent through every retry and the hub
    // would give up with NODE_NOT_CONFIRMED - producing exactly the split state
    // the ordering in this protocol exists to make rare.
    //
    // The restart clock is pushed out too, so the node is still listening when
    // the next retry lands.
    if (_state == State::Confirmed)
    {
        SendConfirm(true);

        _confirmedAt = millis();

        return;
    }


    if (ack.accepted == 0)
    {
        Serial.print(
            "[EspNowNode] The hub refused this node. Reason code: ");

        Serial.println(ack.reason);

        // Nothing is persisted and the node stays unprovisioned. Sweeping again
        // is right: the refusal may be "already adopted", and the household may
        // reset this node and try elsewhere.
        AbandonSession();

        return;
    }


    // --------------------------------------------------
    // The node commits BEFORE the hub does
    // --------------------------------------------------
    //
    // Decision A38-7, and it is deliberate rather than convenient. The
    // alternative - hub first - leaves a registered child Device with no node
    // behind it, which the household can see and which M38 gives them no way to
    // remove. This way a failure between the two leaves an adopted node the hub
    // has no record of: inert, invisible, and recoverable by the node reset
    // milestone 39 adds.
    bool stored = Persist();


    SendConfirm(stored);


    if (!stored)
    {
        Serial.println(
            "[EspNowNode] Could not write to NVS. Reporting the failure and "
            "staying unprovisioned.");

        AbandonSession();

        return;
    }


    _state = State::Confirmed;

    _confirmedAt = millis();
}


// ==================================================
// Outbound
// ==================================================

void EspNowNodeClass::SendResponse()
{
    HsDiscoveryResponse response = {};

    response.header.magic = HS_ESPNOW_MAGIC;
    response.header.protocolVersion = HS_ESPNOW_VERSION;
    response.header.msgType = MSG_DISCOVERY_RESPONSE;
    response.header.sequence = _sequence++;
    response.header.sessionId = _sessionId;

    memcpy(response.nodeMac, _nodeMac, 6);
    memcpy(response.hubMac, _hubMac, 6);


    // This node is the source of truth for its own Device metadata. These three
    // strings are where a sensor's type, its kind key and its display name
    // enter HomeShield.
    HsEspNow::CopyField(
        response.deviceType,
        sizeof(response.deviceType),
        _deviceType);

    HsEspNow::CopyField(
        response.deviceKey,
        sizeof(response.deviceKey),
        _deviceKey);

    HsEspNow::CopyField(
        response.defaultName,
        sizeof(response.defaultName),
        _defaultName);


    esp_err_t result =
        esp_now_send(
            _hubMac,
            (const uint8_t*)&response,
            sizeof(response));


    _responseAttempts++;

    _lastResponseAt = millis();


    Serial.print(
        "[EspNowNode] DISCOVERY_RESPONSE -> ");

    Serial.print(HsEspNow::FormatMac(_hubMac));

    Serial.print(" attempt ");

    Serial.print(_responseAttempts);

    Serial.print(" ch=");

    Serial.print(_channel);

    Serial.print(" send=");

    Serial.println(esp_err_to_name(result));
}


void EspNowNodeClass::SendConfirm(
    bool stored)
{
    HsIdentityConfirm confirm = {};

    confirm.header.magic = HS_ESPNOW_MAGIC;
    confirm.header.protocolVersion = HS_ESPNOW_VERSION;
    confirm.header.msgType = MSG_IDENTITY_CONFIRM;
    confirm.header.sequence = _sequence++;
    confirm.header.sessionId = _sessionId;

    memcpy(confirm.nodeMac, _nodeMac, 6);
    memcpy(confirm.hubMac, _hubMac, 6);

    confirm.stored = stored ? 1 : 0;


    esp_now_send(
        _hubMac,
        (const uint8_t*)&confirm,
        sizeof(confirm));


    Serial.print(
        "[EspNowNode] Sent IDENTITY_CONFIRM, stored=");

    Serial.println(stored ? 1 : 0);
}


// ==================================================
// Relationship check (milestone 40)
// ==================================================

void EspNowNodeClass::SendRelationshipCheck()
{
    if (!_radioStarted) return;


    // The peer carries a channel, and this node is sweeping. Re-pointed only
    // when the sweep has actually moved - AddHubPeer() deletes and re-adds,
    // and doing that on every pass of loop() would churn the peer table for
    // nothing.
    if (!_hubPeerAdded || _peerChannel != _channel)
    {
        if (!AddHubPeer())
        {
            Serial.println(
                "[EspNowNode] Could not add the stored hub as a peer for the "
                "relationship check.");

            return;
        }

        _peerChannel = _channel;
    }


    HsRelationshipCheck check = {};

    check.header.magic = HS_ESPNOW_MAGIC;
    check.header.protocolVersion = HS_ESPNOW_VERSION;
    check.header.msgType = MSG_RELATIONSHIP_CHECK;
    check.header.sequence = _sequence++;
    check.header.sessionId = _sessionId;

    memcpy(check.nodeMac, _nodeMac, 6);
    memcpy(check.hubMac, _hubMac, 6);


    esp_now_send(
        _hubMac,
        (const uint8_t*)&check,
        sizeof(check));
}


void EspNowNodeClass::HandleRelationshipStatus(
    const HsRelationshipStatus& status)
{
    if (_state != State::RelationshipCheck) return;


    // The same three checks the discovery ACK makes, for the same reasons: the
    // session id rejects an answer to a check this node has already finished,
    // the hub MAC rejects an answer from a hub this node did not ask, and the
    // node MAC rejects one meant for a different node.
    //
    // The hub MAC check is the load-bearing one here. Without it another hub
    // in the same house - one this node has never belonged to and whose
    // registry therefore cannot contain it - could revoke it.
    if (status.header.sessionId != _sessionId) return;

    if (memcmp(status.hubMac, _hubMac, 6) != 0) return;

    if (memcmp(status.nodeMac, _nodeMac, 6) != 0) return;


    if (status.verdict == RELATIONSHIP_REVOKED)
    {
        Serial.print(
            "[EspNowNode] Hub ");

        Serial.print(_hubMacText);

        Serial.println(
            " no longer has this node. Clearing the stored relationship.");


        if (!ClearPersisted())
        {
            // Restarting anyway, and saying so. A node that comes back still
            // pointing at a hub that has disowned it will simply ask again at
            // the next boot and be told the same thing - it is recoverable,
            // where a node that refused to restart would sit here forever.
            Serial.println(
                "[EspNowNode] WARNING: the stored relationship could NOT be "
                "verified as cleared. Restarting anyway.");
        }

        _persisted = false;

        Serial.flush();

        ESP.restart();

        return;
    }


    if (status.verdict == RELATIONSHIP_VALID)
    {
        SettleProvisioned(
            "the hub confirmed this node");

        return;
    }


    // A verdict this build does not know. Treated as no answer at all, which
    // is the safe reading: only an explicit REVOKED may unprovision a node,
    // and a value that is not REVOKED is not one.
    Serial.print(
        "[EspNowNode] Unknown relationship verdict ");

    Serial.print(status.verdict);

    Serial.println(". Ignoring it and keeping the stored hub.");
}


void EspNowNodeClass::SettleProvisioned(
    const char* why)
{
    if (_hubPeerAdded)
    {
        esp_now_del_peer(_hubMac);

        _hubPeerAdded = false;
    }


    _state = State::Provisioned;

    _sessionId = 0;


    Serial.print(
        "[EspNowNode] Settled as provisioned to ");

    Serial.print(_hubMacText);

    Serial.print(" - ");

    Serial.print(why);

    Serial.println(".");


    // The radio is left initialised but idle. Bringing it down would gain a
    // node that is about to do nothing anyway, and M38's rule still holds: a
    // provisioned node does not answer discovery requests. State::Provisioned
    // returns from loop() before the inbox is ever drained, so anything that
    // arrives from here on is discarded unread.
}


bool EspNowNodeClass::ClearPersisted()
{
    if (!_preferences.begin(Namespace, false)) return false;


    // The commit marker FIRST, exactly as Persist() writes it LAST. A power
    // cut anywhere after this line leaves an unprovisioned node with a stale
    // hub MAC, which Load() already refuses; the other order would leave one
    // that believes it is adopted by an address it can no longer read.
    _preferences.remove(StateKey);
    _preferences.remove(HubMacKey);
    _preferences.remove(VersionKey);


    bool cleared =
        !_preferences.isKey(StateKey) &&
        !_preferences.isKey(HubMacKey) &&
        !_preferences.isKey(VersionKey);


    _preferences.end();


    return cleared;
}


bool EspNowNodeClass::AddHubPeer()
{
    if (_hubPeerAdded)
    {
        esp_now_del_peer(_hubMac);

        _hubPeerAdded = false;
    }


    esp_now_peer_info_t peer = {};

    memcpy(peer.peer_addr, _hubMac, 6);

    // The channel this node is pinned to, which is the hub's, which is the
    // router's. Zero would mean "whatever the interface is on", and that is the
    // same thing here - but saying it explicitly is what makes the pinning
    // visible to the next reader.
    peer.channel = _channel;

    peer.encrypt = false;

    peer.ifidx = WIFI_IF_STA;


    if (esp_now_add_peer(&peer) != ESP_OK)
    {
        return false;
    }


    _hubPeerAdded = true;

    return true;
}


void EspNowNodeClass::AbandonSession()
{
    if (_hubPeerAdded)
    {
        esp_now_del_peer(_hubMac);

        _hubPeerAdded = false;
    }


    memset(_hubMac, 0, sizeof(_hubMac));

    _hubMacText = "";

    _sessionId = 0;

    _sequence = 0;

    _responseAttempts = 0;

    _lastResponseAt = 0;

    _state = State::Discovery;

    _channelSteppedAt = millis();
}


// ==================================================
// NVS
// ==================================================

bool EspNowNodeClass::Persist()
{
    if (!_preferences.begin(Namespace, false)) return false;


    String hubMacText =
        HsEspNow::FormatMac(_hubMac);


    bool ok =
        _preferences.putString(HubMacKey, hubMacText) > 0;

    ok = ok &&
        _preferences.putInt(VersionKey, CurrentVersion) > 0;


    // Read back before the marker goes down. A write that reports success and
    // did not land would otherwise produce a node that is provisioned to a hub
    // address it cannot read.
    ok = ok &&
        _preferences.getString(HubMacKey, "") == hubMacText;


    // LAST. Everything above is inert until this exists.
    if (ok)
    {
        ok = _preferences.putInt(StateKey, 1) > 0;
    }


    _preferences.end();


    if (ok)
    {
        _hubMacText = hubMacText;
    }


    return ok;
}


bool EspNowNodeClass::Load()
{
    if (!_preferences.begin(Namespace, true)) return false;


    int state =
        _preferences.getInt(StateKey, 0);

    String hubMacText =
        _preferences.getString(HubMacKey, "");

    _preferences.end();


    // The marker is the whole test. Keys without it are the residue of a write
    // that did not finish, and a node carrying them is unprovisioned.
    if (state != 1) return false;


    if (!HsEspNow::ParseMac(hubMacText, _hubMac)) return false;


    _hubMacText = hubMacText;

    return true;
}


bool EspNowNodeClass::IsProvisioned() const
{
    // Milestone 40. The stored relationship, not the loop's state.
    //
    // It used to be State::Provisioned, which was the same thing when a node
    // booted straight into it. A node is now in RelationshipCheck for a few
    // seconds first, and it IS adopted throughout - the check can only take
    // that away, never grant it - so answering "no" during those seconds would
    // tell a sketch something untrue.
    return _persisted;
}


const String& EspNowNodeClass::HubMac() const
{
    return _hubMacText;
}


String EspNowNodeClass::NodeMac() const
{
    return HsEspNow::FormatMac(_nodeMac);
}
