#include "EspNowNode.h"
#include "Debug.h"
#include "DeviceIdentity.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

// SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP, which EnterDeepSleep() branches on.
// Included explicitly rather than relied on from esp_sleep.h: if it arrived
// only indirectly and that ever stopped being true, the #if would quietly
// evaluate as 0 and sensor inputs would stop waking the node - with nothing in
// the build to say so.
#include <soc/soc_caps.h>


EspNowNodeClass EspNowNode;


// ============================================================
// What survives deep sleep (milestone 41)
// ============================================================
//
// RTC fast memory, not NVS. Both of these change on most wakes,
// and an hourly NVS write is 8 760 flash writes a year for values
// whose only job is to outlive a sleep - which RTC memory does for
// free and flash does not do forever.
//
// s_rtcMagic is what tells "this survived a deep sleep" from "this
// is a cold boot and the RAM is whatever it is". A hard reset or a
// power cut clears RTC memory, and both of those SHOULD forget the
// recovery schedule: a node somebody just power-cycled is starting
// over, and asking it to wait five minutes would be rude.
//
// The channel is mirrored here as well as in NVS so a timer wake
// reads no flash at all on the happy path.
static const uint32_t RTC_STATE_MAGIC = 0x4D343100;   // "M41"

RTC_DATA_ATTR static uint32_t s_rtcMagic      = 0;
RTC_DATA_ATTR static uint8_t  s_rtcRecovering = 0;
RTC_DATA_ATTR static uint8_t  s_rtcChannel    = 0;


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


    // --------------------------------------------------
    // Milestone 41: why are we awake, and is the button down?
    // --------------------------------------------------
    //
    // Before the radio, before NVS is read for the hub, and before anything
    // that could take time: a household holding the reset button wants this
    // node to forget its hub, and making them wait through a channel sweep
    // first would make the button feel broken.
    if (ClassifyWakeAndCheckReset())
    {
        // Unprovisioned and about to restart. Nothing else may run.
        return;
    }


    // --------------------------------------------------
    // Say why we are awake, and what the inputs read
    // --------------------------------------------------
    //
    // Added because its absence made a field fault unresolvable from the log:
    // "woken from deep sleep" could equally have been the hourly timer or a
    // sensor, and those are completely different diagnoses. The pin levels
    // are printed beside it so a sensor that is not tracking its float is
    // visible immediately rather than being inferred from a reported state.
    DEBUG_LOG_PRINT(
        "[EspNowNode] Wake reason: ");

    switch (_wakeReason)
    {
        case WAKE_POWER_ON: DEBUG_LOG_PRINT("POWER_ON / reset"); break;
        case WAKE_SENSOR:   DEBUG_LOG_PRINT("SENSOR GPIO");       break;
        case WAKE_BUTTON:   DEBUG_LOG_PRINT("RESET BUTTON");      break;
        case WAKE_TIMER:    DEBUG_LOG_PRINT("RTC TIMER");         break;
        default:            DEBUG_LOG_PRINT("unknown");           break;
    }


    if (_wakeGpioStatus != 0)
    {
        DEBUG_LOG_PRINT(" fired by");

        for (int i = 0; i < _wakePinCount; i++)
        {
            if ((_wakeGpioStatus & (1ULL << _wakePins[i])) == 0) continue;

            DEBUG_LOG_PRINT(" gpio");

            DEBUG_LOG_PRINT(_wakePins[i]);
        }
    }

    if (_wakePinCount > 0)
    {
        DEBUG_LOG_PRINT("  [");

        for (int i = 0; i < _wakePinCount; i++)
        {
            if (i > 0) DEBUG_LOG_PRINT(" ");

            DEBUG_LOG_PRINT("gpio");

            DEBUG_LOG_PRINT(_wakePins[i]);

            DEBUG_LOG_PRINT("=");

            DEBUG_LOG_PRINT(digitalRead(_wakePins[i]) == LOW ? "LOW" : "HIGH");

            DEBUG_LOG_PRINT(_wakePull[i] == HS_WAKE_PULL_DOWN ? "/pd" : "/pu");
        }

        DEBUG_LOG_PRINT("]");
    }

    DEBUG_LOG("");


    // --------------------------------------------------
    // The firing pin is no longer at the level that fired it
    // --------------------------------------------------
    //
    // Said out loud because it is a HARDWARE report, not a firmware one, and
    // the two are easy to confuse. The wake worked; the sensor let go before
    // the chip finished booting, which takes on the order of a hundred
    // milliseconds. A float switch that does that is bouncing, momentary, or
    // wired through something that cannot hold the level.
    //
    // The node still reports what it READS, because the reported state is a
    // level and reporting one the tank is not in would be worse than
    // reporting a stale one.
    for (int i = 0; i < _wakePinCount; i++)
    {
        if ((_wakeGpioStatus & (1ULL << _wakePins[i])) == 0) continue;

        bool nowLow = digitalRead(_wakePins[i]) == LOW;

        bool firedOnHigh = _wakePull[i] == HS_WAKE_PULL_DOWN;

        // A pull-down pin rests LOW, so it can only have fired on HIGH; a
        // pull-up pin rests HIGH and can only have fired on LOW.
        if (firedOnHigh != nowLow) continue;

        DEBUG_LOG_PRINT(
            "[EspNowNode] NOTE: gpio");

        DEBUG_LOG_PRINT(_wakePins[i]);

        DEBUG_LOG(
            " caused this wake but is already back at its resting level. The "
            "wake path is working; the input did not hold long enough to be "
            "read. Check for a bouncing or momentary contact.");
    }


    DEBUG_VALUE("[EspNowNode] Node MAC", HsEspNow::FormatMac(_nodeMac));


    if (Load())
    {
        _persisted = true;

        DEBUG_LOG_PRINT(
            "[EspNowNode] Provisioned. Hub: ");

        DEBUG_LOG_PRINT(
            _hubMacText);

        DEBUG_LOG_PRINT(", cached channel ");

        DEBUG_LOG_PRINT(_hubChannel == 0 ? -1 : (int)_hubChannel);

        DEBUG_LOG_PRINT(", recovering=");

        DEBUG_LOG(IsRecovering() ? 1 : 0);


        // --------------------------------------------------
        // Milestone 41: the relationship check is a BOOT thing
        // --------------------------------------------------
        //
        // M40's check costs two full channel sweeps - about 16 s with the
        // radio up - and it asks a question whose answer changes roughly
        // never. Running it on every hourly wake would dominate the power
        // budget of a node whose entire purpose is to be cheap.
        //
        // So it runs on a COLD BOOT only. M40's guarantee is untouched:
        // silence never unprovisions, only an explicit REVOKED does, and a
        // node still asks every time it is genuinely restarted - which is
        // what a household does to a sensor that is misbehaving, and what
        // happens whenever its battery is changed.
        if (_wakeReason != WAKE_POWER_ON)
        {
            StartRadio();

            _state = State::Provisioned;

            DEBUG_LOG(
                "[EspNowNode] Woken from deep sleep; skipping the relationship "
                "check and reporting straight away.");

            return;
        }


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


        DEBUG_LOG(
            "[EspNowNode] Verifying the stored hub relationship before "
            "settling.");


        StartRadio();

        // Channel 1 is already set and dwelling; the sweep in loop() sends on
        // every channel AFTER it steps, so the first one is sent here.
        SendRelationshipCheck();

        return;
    }


    DEBUG_LOG(
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

    DEBUG_VALUE("[EspNowNode] Power save disabled", esp_err_to_name(ps));


    _channel = FirstChannel;

    esp_err_t channelResult =
        esp_wifi_set_channel(
            _channel,
            WIFI_SECOND_CHAN_NONE);

    _channelSteppedAt = millis();


    esp_err_t initResult = esp_now_init();

    if (initResult != ESP_OK)
    {
        DEBUG_LOG_PRINT(
            "[EspNowNode] esp_now_init() FAILED: ");

        DEBUG_LOG(esp_err_to_name(initResult));

        DEBUG_LOG(
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


    DEBUG_LOG_PRINT(
        "[EspNowNode] ESP-NOW init=");

    DEBUG_LOG_PRINT(esp_err_to_name(initResult));

    DEBUG_LOG_PRINT(" recv_cb=");

    DEBUG_LOG_PRINT(esp_err_to_name(cbResult));

    DEBUG_LOG_PRINT(" send_cb=");

    DEBUG_LOG_PRINT(esp_err_to_name(sendCbResult));

    DEBUG_LOG_PRINT(" set_channel(");

    DEBUG_LOG_PRINT(_channel);

    DEBUG_LOG_PRINT(")=");

    DEBUG_LOG(esp_err_to_name(channelResult));


    DEBUG_LOG_PRINT(
        "[EspNowNode] Sweeping channels ");

    DEBUG_LOG_PRINT(FirstChannel);

    DEBUG_LOG_PRINT("-");

    DEBUG_LOG_PRINT(LastChannel);

    DEBUG_LOG_PRINT(", dwelling ");

    DEBUG_LOG_PRINT(ChannelDwell);

    DEBUG_LOG(" ms on each.");
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
        // MSG_NODE_REPORT_ACK is deliberately NOT handled here. It is drained
        // by DeliverReport(), which owns the radio for the few hundred
        // milliseconds a report takes and is the only thing that ever asked
        // for one. An ACK reaching loop() is a duplicate of one already acted
        // on, and dropping it is correct.

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
                DEBUG_LOG(
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

            DEBUG_LOG(
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
        DEBUG_LOG_PRINT(
            "[EspNowNode] set_channel(");

        DEBUG_LOG_PRINT(_channel);

        DEBUG_LOG_PRINT(") FAILED: ");

        DEBUG_LOG(esp_err_to_name(result));
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
        DEBUG_LOG(
            "[EspNowNode] RADIO NOT STARTED - ESP-NOW failed to initialise. "
            "This node cannot be discovered until it is restarted.");

        return;
    }


    DEBUG_LOG_PRINT("[EspNowNode] listening ch=");

    DEBUG_LOG_PRINT(_channel);

    DEBUG_LOG_PRINT(" (radio ch=");

    DEBUG_LOG_PRINT(primary);

    DEBUG_LOG_PRINT(") sweeps=");

    DEBUG_LOG_PRINT(_sweepsCompleted);

    DEBUG_LOG_PRINT(" framesHeard=");

    DEBUG_LOG_PRINT(_framesHeard);

    DEBUG_LOG_PRINT(" forOtherNodes=");

    DEBUG_LOG_PRINT(_framesForOthers);

    DEBUG_LOG_PRINT(" sent=");

    DEBUG_LOG_PRINT(_sendsCompleted);

    DEBUG_LOG_PRINT(" acked=");

    DEBUG_LOG_PRINT(_sendsCompleted - _sendsFailed);

    DEBUG_LOG_PRINT(" state=");

    switch (_state)
    {
        case State::Discovery:    DEBUG_LOG("DISCOVERY"); break;
        case State::Responding:   DEBUG_LOG("RESPONDING"); break;
        case State::Confirmed:    DEBUG_LOG("CONFIRMED"); break;
        case State::RelationshipCheck:
                                  DEBUG_LOG("RELATIONSHIP_CHECK"); break;
        case State::Provisioned:  DEBUG_LOG("PROVISIONED"); break;
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


    // Milestone 41. Recorded so DeliverReport()'s wait can end the moment the
    // radio says the frame was not acknowledged, instead of spending the whole
    // retryIntervalMs waiting for an answer to something that provably never
    // reached the hub. Three failed attempts then cost a few milliseconds
    // rather than 1.2 s of radio-on time.
    _lastSendOk = ok;

    _lastSendReported = true;


    // Every one of them, for now. There are only ever five per discovery
    // cycle, and each is a direct answer to the question the hub's silence
    // raised.
    DEBUG_LOG_PRINT(
        "[EspNowNode] -> send callback #");

    DEBUG_LOG_PRINT(_sendsCompleted);

    if (ok)
    {
        DEBUG_LOG(
            " = SUCCESS (the hub's radio acknowledged this frame)");
    }
    else
    {
        DEBUG_LOG(
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
            DEBUG_LOG_PRINT(
                "[EspNowNode] Heard a discovery request for ");

            DEBUG_LOG_PRINT(HsEspNow::FormatMac(request.targetMac));

            DEBUG_LOG_PRINT(", which is not this node (");

            DEBUG_LOG_PRINT(HsEspNow::FormatMac(_nodeMac));

            DEBUG_LOG("). Ignoring, as designed.");
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
        DEBUG_LOG(
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


    DEBUG_LOG_PRINT(
        "[EspNowNode] Discovery request from hub ");

    DEBUG_LOG_PRINT(_hubMacText);

    DEBUG_LOG_PRINT(" on channel ");

    DEBUG_LOG(_channel);


    if (!AddHubPeer())
    {
        DEBUG_LOG(
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
        DEBUG_VALUE("[EspNowNode] The hub refused this node. Reason code", ack.reason);

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
        DEBUG_LOG(
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


    DEBUG_LOG_PRINT(
        "[EspNowNode] DISCOVERY_RESPONSE -> ");

    DEBUG_LOG_PRINT(HsEspNow::FormatMac(_hubMac));

    DEBUG_LOG_PRINT(" attempt ");

    DEBUG_LOG_PRINT(_responseAttempts);

    DEBUG_LOG_PRINT(" ch=");

    DEBUG_LOG_PRINT(_channel);

    DEBUG_LOG_PRINT(" send=");

    DEBUG_LOG(esp_err_to_name(result));
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


    DEBUG_LOG_PRINT(
        "[EspNowNode] Sent IDENTITY_CONFIRM, stored=");

    DEBUG_LOG(stored ? 1 : 0);
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
            DEBUG_LOG(
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
        DEBUG_LOG_PRINT(
            "[EspNowNode] Hub ");

        DEBUG_LOG_PRINT(_hubMacText);

        DEBUG_LOG(
            " no longer has this node. Clearing the stored relationship.");


        if (!ClearPersisted())
        {
            // Restarting anyway, and saying so. A node that comes back still
            // pointing at a hub that has disowned it will simply ask again at
            // the next boot and be told the same thing - it is recoverable,
            // where a node that refused to restart would sit here forever.
            DEBUG_LOG(
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
    DEBUG_LOG_PRINT(
        "[EspNowNode] Unknown relationship verdict ");

    DEBUG_LOG_PRINT(status.verdict);

    DEBUG_LOG(". Ignoring it and keeping the stored hub.");
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


    DEBUG_LOG_PRINT(
        "[EspNowNode] Settled as provisioned to ");

    DEBUG_LOG_PRINT(_hubMacText);

    DEBUG_LOG_PRINT(" - ");

    DEBUG_LOG_PRINT(why);

    DEBUG_LOG(".");


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

    // Milestone 41. The channel cache belongs to the relationship being
    // cleared. Removed after the marker, with the rest of the residue.
    _preferences.remove(HubChannelKey);


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


    // Milestone 41. The channel this node locked onto when it heard the hub,
    // which is the router's and therefore the hub's. Written here because this
    // is the one moment the node knows it for certain.
    //
    // NOT part of `ok`: it is a cache, and a node that could not write it is
    // still correctly adopted - it simply sweeps on its first wake.
    if (_channel >= FirstChannel && _channel <= LastChannel)
    {
        _preferences.putUChar(HubChannelKey, _channel);

        s_rtcChannel = _channel;

        _hubChannel = _channel;
    }


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

    // Milestone 41. A node adopted before this milestone has no stored
    // channel, and 0 is exactly the right answer for it: sweep once, learn,
    // remember. No migration and no version bump - an absent cache is not a
    // broken record.
    uint8_t storedChannel =
        _preferences.getUChar(HubChannelKey, 0);

    _preferences.end();


    // The marker is the whole test. Keys without it are the residue of a write
    // that did not finish, and a node carrying them is unprovisioned.
    if (state != 1) return false;


    if (!HsEspNow::ParseMac(hubMacText, _hubMac)) return false;


    _hubMacText = hubMacText;


    // The RTC copy wins when it is valid: it is either the same value or a
    // newer one that a sweep learned since the last cold boot.
    if (s_rtcMagic == RTC_STATE_MAGIC &&
        s_rtcChannel >= FirstChannel &&
        s_rtcChannel <= LastChannel)
    {
        _hubChannel = s_rtcChannel;
    }
    else if (storedChannel >= FirstChannel &&
             storedChannel <= LastChannel)
    {
        _hubChannel = storedChannel;
    }
    else
    {
        _hubChannel = 0;
    }


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


// ==================================================
// Milestone 41 - reporting and deep sleep
// ==================================================

void EspNowNodeClass::configureSleep(
    const EspNowNodeSleep& sleep)
{
    _sleep = sleep;


    // Guarded rather than trusted. Every one of these has a value that would
    // brick a battery node silently: a zero normal interval spins the radio
    // forever, a zero attempt count never sends at all, and a zero recovery
    // interval turns a hub outage into a flat battery by morning.
    if (_sleep.normalWakeSeconds == 0)   _sleep.normalWakeSeconds = 3600;

    if (_sleep.recoveryWakeSeconds == 0) _sleep.recoveryWakeSeconds = 300;

    if (_sleep.maxAttempts < 1)          _sleep.maxAttempts = 1;

    if (_sleep.retryIntervalMs == 0)     _sleep.retryIntervalMs = 400;
}


bool EspNowNodeClass::addWakePin(
    uint8_t gpio,
    HsWakePull pull)
{
    if (_wakePinCount >= MaxWakePins)
    {
        DEBUG_LOG_PRINT(
            "[EspNowNode] Wake pin ");

        DEBUG_LOG_PRINT(gpio);

        DEBUG_LOG(
            " was refused: the wake table is full.");

        return false;
    }


#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    // --------------------------------------------------
    // Not every GPIO can end a deep sleep
    // --------------------------------------------------
    //
    // On the ESP32-C3 exactly six can - GPIO 0 to 5 - and the SDK names them
    // itself in soc_caps.h. Refused here, loudly, rather than accepted and
    // quietly unable to fire: esp_deep_sleep_enable_gpio_wakeup() answers
    // ESP_ERR_INVALID_ARG for any other pin, and a sensor that reads correctly
    // on every wake while being incapable of CAUSING one looks exactly like a
    // sensor that works, right up until somebody notices the state is an hour
    // stale.
    if (((1ULL << gpio) & SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK) == 0)
    {
        DEBUG_LOG_PRINT(
            "[EspNowNode] GPIO ");

        DEBUG_LOG_PRINT(gpio);

        DEBUG_LOG(
            " CANNOT wake this chip from deep sleep and was refused as a wake "
            "source. Only GPIO 0-5 can on the ESP32-C3 "
            "(SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK).");

        DEBUG_LOG(
            "[EspNowNode]   The pin is still read on every wake, so its state "
            "IS reported - but only when the RTC timer or another wake source "
            "brings the node up. Move the sensor to GPIO 0-5 for immediate "
            "reporting.");

        return false;
    }
#endif


    for (int i = 0; i < _wakePinCount; i++)
    {
        if (_wakePins[i] == gpio)
        {
            // Re-declaring a pin is allowed and updates its wiring, which is
            // friendlier than refusing a sketch that says the same thing twice.
            _wakePull[i] = pull;

            return true;
        }
    }


    _wakePull[_wakePinCount] = pull;

    _wakePins[_wakePinCount] = gpio;

    _wakePinCount++;

    return true;
}


HsNodeWake EspNowNodeClass::WakeReason() const
{
    return _wakeReason;
}


bool EspNowNodeClass::IsRecovering() const
{
    return
        s_rtcMagic == RTC_STATE_MAGIC &&
        s_rtcRecovering == 1;
}


void EspNowNodeClass::ReleaseWakePinHolds()
{
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    // --------------------------------------------------
    // Configure FIRST, release SECOND. That order is the documented one.
    // --------------------------------------------------
    //
    // gpio_hold_dis() says it outright:
    //
    //   "When the chip is woken up from peripheral power-down sleep, the gpio
    //    will be set to the default mode ... the gpio should be configured to
    //    a known state BEFORE this function is called."
    //
    // While a pad is held it ignores configuration changes, but the writes
    // still land in the GPIO registers - so writing the configuration first
    // and releasing afterwards means the pad adopts what is already waiting
    // for it, with no intermediate state at all.
    //
    // Releasing first, which is what this function did before, snaps every pad
    // to its default (input, no pull) for as long as the next few instructions
    // take. A float that was driving its pin HIGH meets a pad that is briefly
    // floating and then pulled down, and the value read a moment later is the
    // pull-down's, not the sensor's. That is precisely the fault this was
    // written to fix and it reproduced it one step further along: the node
    // woke on the float - the wake reason said SENSOR GPIO - and then read
    // both floats LOW and reported EMPTY.
    for (int i = 0; i < _wakePinCount; i++)
    {
        gpio_num_t pin = (gpio_num_t)_wakePins[i];

        gpio_set_direction(pin, GPIO_MODE_INPUT);

        if (_wakePull[i] == HS_WAKE_PULL_DOWN)
        {
            gpio_pulldown_en(pin);

            gpio_pullup_dis(pin);
        }
        else
        {
            gpio_pullup_en(pin);

            gpio_pulldown_dis(pin);
        }
    }


    // Now let go. Each pad takes up the configuration written above.
    for (int i = 0; i < _wakePinCount; i++)
    {
        gpio_hold_dis((gpio_num_t)_wakePins[i]);
    }

    gpio_deep_sleep_hold_dis();


    if (_wakePinCount > 0)
    {
        // The pull needs a moment to establish before the first read means
        // anything - the same allowance the reset-button check already makes.
        delay(20);
    }
#endif
}


// --------------------------------------------------
// Why this boot happened, and the reset button
// --------------------------------------------------
//
// Returns true when the node has been unprovisioned and a restart is coming,
// which is the caller's signal to do nothing else at all.
bool EspNowNodeClass::ClassifyWakeAndCheckReset()
{
    esp_sleep_wakeup_cause_t cause =
        esp_sleep_get_wakeup_cause();


    // WHICH pin fired, latched by the hardware. Read before anything
    // reconfigures a pad, and before the holds are released.
    //
    // This is the number that separates "the wake never happened" from "the
    // wake happened and the level was gone by the time we looked", which are
    // opposite faults that produce an identical symptom: a sensor stuck in one
    // state. Without it that distinction cannot be made from a log at all.
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    _wakeGpioStatus = esp_sleep_get_gpio_wakeup_status();
#endif


    // Then the pads: configured and released before any digitalRead in this
    // function or anywhere after it.
    ReleaseWakePinHolds();


    // NOT guarded with #if defined(). These are values of the esp_sleep_source_t
    // ENUM, not preprocessor macros, so `#if defined(ESP_SLEEP_WAKEUP_GPIO)` is
    // false on every target and would silently compile the GPIO case away -
    // leaving every sensor wake classified as a cold boot, and every one of them
    // running M40's 16-second relationship check on battery.
    //
    // The enum is common to all ESP32 targets, so both spellings are always
    // declared whether or not the part implements them.
    switch (cause)
    {
        case ESP_SLEEP_WAKEUP_TIMER:
            _wakeReason = WAKE_TIMER;
            break;

        case ESP_SLEEP_WAKEUP_GPIO:
            _wakeReason = WAKE_SENSOR;
            break;

        case ESP_SLEEP_WAKEUP_EXT1:
            // The ESP32 classic's spelling of the same event. Carried so this
            // file is not silently wrong if a node is ever built on one.
            _wakeReason = WAKE_SENSOR;
            break;

        default:
            // ESP_SLEEP_WAKEUP_UNDEFINED. Not a deep-sleep wake: a cold boot,
            // a hard reset, or the first boot after flashing.
            _wakeReason = WAKE_POWER_ON;
            break;
    }


    // --------------------------------------------------
    // The reset / manual wake input
    // --------------------------------------------------
    //
    // Checked on EVERY boot, not only on a GPIO wake. A household holding the
    // button and then power-cycling the node means the same thing as holding
    // it after a timer wake, and a reset that only worked from one of the two
    // would be a reset nobody trusts.
    if (_sleep.resetPin < 0)
    {
        return false;
    }


    uint8_t resetPin = (uint8_t)_sleep.resetPin;

    pinMode(
        resetPin,
        _sleep.resetActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);

    // The pull needs a moment to establish before the first read means
    // anything.
    delay(20);


    int activeLevel = _sleep.resetActiveLow ? LOW : HIGH;

    if (digitalRead(resetPin) != activeLevel)
    {
        return false;
    }


    // It is down. Distinguish a hold from a tap, which is a manual report and
    // not a reset.
    DEBUG_LOG_PRINT(
        "[EspNowNode] Reset input is active. Hold for ");

    DEBUG_LOG_PRINT(_sleep.resetHoldMs);

    DEBUG_LOG(" ms to clear this node's hub.");


    unsigned long pressedAt = millis();

    while (digitalRead(resetPin) == activeLevel)
    {
        if (millis() - pressedAt >= _sleep.resetHoldMs) break;

        delay(10);
    }


    if (digitalRead(resetPin) != activeLevel)
    {
        // Released early. This is the approved manual wake: the node carries
        // on and reports, and says so rather than looking like it ignored the
        // button.
        _wakeReason = WAKE_BUTTON;

        DEBUG_LOG(
            "[EspNowNode] Released early - treating it as a manual report.");

        return false;
    }


    // --------------------------------------------------
    // Held. Clear the relationship, exactly as M40 does.
    // --------------------------------------------------
    //
    // Marker first, verified, then restart - and restart even when the clear
    // could not be verified, for M40's reason: a node that refused to restart
    // would sit here forever, where one that restarts asks its hub again and
    // is either confirmed or revoked.
    DEBUG_LOG(
        "[EspNowNode] Reset held. Clearing the stored hub relationship.");


    if (!ClearPersisted())
    {
        DEBUG_LOG(
            "[EspNowNode] WARNING: the stored relationship could NOT be "
            "verified as cleared. Restarting anyway.");
    }


    _persisted = false;

    // The recovery schedule belonged to a relationship that no longer exists.
    s_rtcMagic = 0;

    s_rtcRecovering = 0;

    s_rtcChannel = 0;


    Serial.flush();

    ESP.restart();

    return true;
}


// --------------------------------------------------
// One attempt on one channel
// --------------------------------------------------

bool EspNowNodeClass::SendReportOnChannel(
    uint8_t channel,
    int state)
{
    if (!_radioStarted) return false;


    if (channel >= FirstChannel && channel <= LastChannel)
    {
        if (_channel != channel)
        {
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

            _channel = channel;
        }
    }


    // AddHubPeer() deletes and re-adds, so it is called only when the peer is
    // missing or is pointed at a channel we have since left.
    if (!_hubPeerAdded || _peerChannel != _channel)
    {
        if (!AddHubPeer()) return false;

        _peerChannel = _channel;
    }


    HsNodeReport report = {};

    report.header.magic = HS_ESPNOW_MAGIC;
    report.header.protocolVersion = HS_ESPNOW_VERSION;
    report.header.msgType = MSG_NODE_REPORT;
    report.header.sequence = _sequence++;
    report.header.sessionId = _sessionId;

    memcpy(report.nodeMac, _nodeMac, 6);
    memcpy(report.hubMac, _hubMac, 6);

    report.state = (int16_t)state;

    report.wakeReason = (uint8_t)_wakeReason;

    // Battery is a MODULE fact in HomeShield and a node is a Device, so there
    // is nowhere correct to record it. Carried as "not available" rather than
    // omitted, so the field exists the day that changes.
    report.battery = -1;

    report.recovering = IsRecovering() ? 1 : 0;


    _lastSendReported = false;

    _lastSendOk = false;

    _reportAcked = false;


    esp_err_t result =
        esp_now_send(
            _hubMac,
            (const uint8_t*)&report,
            sizeof(report));

    if (result != ESP_OK) return false;


    // --------------------------------------------------
    // Wait for the ACK
    // --------------------------------------------------
    //
    // Polled rather than handed to loop(): this class owns the radio for the
    // few hundred milliseconds a report takes, and there is nothing else for
    // the node to be doing.
    unsigned long waitStarted = millis();

    while (millis() - waitStarted < _sleep.retryIntervalMs)
    {
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
                    MSG_NODE_REPORT_ACK,
                    sizeof(HsNodeReportAck)))
            {
                HsNodeReportAck ack;

                memcpy(&ack, frame, sizeof(ack));


                // The same three checks every other inbound frame in this
                // protocol makes, for the same three reasons: the session id
                // rejects an answer to a report this node has finished with,
                // the hub MAC rejects an answer from a hub this node does not
                // belong to, and the node MAC rejects one meant for whichever
                // node the hub spoke to last.
                if (ack.header.sessionId == _sessionId &&
                    memcmp(ack.hubMac, _hubMac, 6) == 0 &&
                    memcmp(ack.nodeMac, _nodeMac, 6) == 0)
                {
                    if (ack.accepted == 1)
                    {
                        _reportAcked = true;

                        return true;
                    }


                    // Refused. There is one reason a hub refuses a report -
                    // the MAC is not in its registry - and it will refuse the
                    // next attempt for the same reason, so this ends the wake.
                    //
                    // It does NOT unprovision the node. M40 owns that
                    // question and answers it with a REVOKED verdict at the
                    // next cold boot; a refused report is not that, and a node
                    // that unprovisioned itself on one packet would be one
                    // malformed frame away from leaving the household.
                    DEBUG_VALUE(
                        "[EspNowNode] The hub REFUSED this report. Reason code",
                        ack.reason);

                    return false;
                }
            }
        }


        // The radio has already said the frame was not acknowledged at the
        // 802.11 layer. Nothing is coming; stop waiting.
        if (_lastSendReported && !_lastSendOk) break;


        delay(2);
    }


    return false;
}


// --------------------------------------------------
// Up to maxAttempts, then give up until the next wake
// --------------------------------------------------

bool EspNowNodeClass::DeliverReport(
    int state)
{
    // The node owns this exchange, so it generates the session id - as M40's
    // relationship check does, and for the same reason.
    do
    {
        _sessionId = esp_random();
    }
    while (_sessionId == 0);


    uint8_t known = CachedChannel();


    for (int attempt = 1; attempt <= _sleep.maxAttempts; attempt++)
    {
        DEBUG_LOG_PRINT(
            "[EspNowNode] NODE_REPORT state=");

        DEBUG_LOG_PRINT(state);

        DEBUG_LOG_PRINT(" attempt ");

        DEBUG_LOG_PRINT(attempt);

        DEBUG_LOG_PRINT("/");

        DEBUG_LOG(_sleep.maxAttempts);


        if (known != 0)
        {
            if (SendReportOnChannel(known, state))
            {
                RememberChannel(known);

                return true;
            }


            // --------------------------------------------------
            // One failure is enough to distrust the cache
            // --------------------------------------------------
            //
            // The cached channel is the router's, and a router that has moved
            // is the ordinary reason for a first failure. Every remaining
            // attempt therefore sweeps, which is what makes a node recover
            // from a channel change by itself instead of failing hourly
            // forever.
            known = 0;

            continue;
        }


        // The sweep. One send per channel, each with its own ACK wait, and
        // bounded by the channel list - this is ONE attempt, not thirteen.
        for (uint8_t channel = FirstChannel; channel <= LastChannel; channel++)
        {
            if (SendReportOnChannel(channel, state))
            {
                RememberChannel(channel);

                return true;
            }
        }
    }


    return false;
}


uint8_t EspNowNodeClass::CachedChannel() const
{
    if (_hubChannel >= FirstChannel && _hubChannel <= LastChannel)
    {
        return _hubChannel;
    }

    return 0;
}


void EspNowNodeClass::RememberChannel(
    uint8_t channel)
{
    if (channel < FirstChannel || channel > LastChannel) return;


    // RTC first and always: it is free, and it is what the next wake reads.
    s_rtcChannel = channel;

    s_rtcMagic = RTC_STATE_MAGIC;


    if (_hubChannel == channel) return;


    _hubChannel = channel;


    // NVS only when it actually moved. A node that reports hourly on a stable
    // router writes this once in its life.
    if (_preferences.begin(Namespace, false))
    {
        _preferences.putUChar(HubChannelKey, channel);

        _preferences.end();
    }
}


// --------------------------------------------------
// The whole of an adopted node's working life
// --------------------------------------------------

void EspNowNodeClass::reportAndSleep(
    int state)
{
    // An unadopted node must stay awake and discoverable. M38's "Turn on the
    // sensor node to continue" is only true because of this line.
    if (!_persisted) return;


    // Still settling M40's relationship check. It runs on a cold boot only and
    // takes about 16 s; sleeping through it would mean never finishing it, and
    // a node that has been revoked would keep reporting to a hub that has
    // disowned it.
    if (_state != State::Provisioned) return;


    bool delivered = DeliverReport(state);


    if (delivered)
    {
        DEBUG_LOG(
            "[EspNowNode] Report acknowledged by the hub.");
    }
    else
    {
        DEBUG_LOG_PRINT(
            "[EspNowNode] No acknowledgement after ");

        DEBUG_LOG_PRINT(_sleep.maxAttempts);

        DEBUG_LOG(
            " attempts. Sleeping on the recovery schedule.");
    }


    EnterDeepSleep(delivered);
}


// --------------------------------------------------
// Down
// --------------------------------------------------

void EspNowNodeClass::EnterDeepSleep(
    bool delivered)
{
    // The recovery flag, which is the whole of the escalation: delivered means
    // the normal hour, not delivered means five minutes and keep trying.
    s_rtcRecovering = delivered ? 0 : 1;

    s_rtcMagic = RTC_STATE_MAGIC;


    uint32_t seconds =
        delivered
            ? _sleep.normalWakeSeconds
            : _sleep.recoveryWakeSeconds;


    esp_sleep_enable_timer_wakeup(
        (uint64_t)seconds * 1000000ULL);


    // --------------------------------------------------
    // The GPIO wake mask: every pin, armed AWAY from where it is
    // --------------------------------------------------
    //
    // The rule is one line: each wake pin is armed for the level it is NOT at,
    // so every one of them wakes the node on its next CHANGE, whichever
    // direction that change happens to be. Recomputed from a live read on
    // every single sleep, because the level that means "something happened"
    // depends entirely on where the pin is right now.
    //
    // ------------------------------------------------------------------
    // Per-pin levels, which the obvious API does not give you
    // ------------------------------------------------------------------
    //
    // esp_deep_sleep_enable_gpio_wakeup() takes ONE mode for the whole mask,
    // and that single fact caused two failed attempts at this function. A door
    // sensor has one sensor pin and never notices; a tank has two floats that
    // routinely rest at OPPOSITE levels, and then one shared level cannot
    // express what both of them need:
    //
    //   NORMAL:  GPIO3 LOW  (wants a HIGH wake - the tank filling)
    //            GPIO1 HIGH (wants a LOW  wake - the tank emptying)
    //
    // Dropping the pin that loses is what the previous version did, and it is
    // why a tank could fill from NORMAL to FULL and not be noticed until the
    // hourly timer.
    //
    // The limitation is in that WRAPPER, not in the chip. driver/gpio.h
    // exposes the primitive the wrapper is built on:
    //
    //   esp_err_t gpio_deep_sleep_wakeup_enable(gpio_num_t, gpio_int_type_t);
    //
    // which takes the level PER PIN. So the wrapper is called to arm the mask
    // and set the sleep's wakeup trigger, and then each pin's own level is
    // asserted individually. Nothing is dropped, and GPIO3 can wait for a
    // rising edge while GPIO1 waits for a falling one in the same sleep.
    //
    // The pull-and-hold from the door-sensor fix is what makes this safe: the
    // SDK would otherwise pick each pin's resistor from the wake mode and undo
    // it. See below.
    uint64_t maskHigh = 0;   // pins resting LOW  -> arm for HIGH
    uint64_t maskLow = 0;    // pins resting HIGH -> arm for LOW

    for (int i = 0; i < _wakePinCount; i++)
    {
        uint8_t pin = _wakePins[i];

        if (digitalRead(pin) == LOW) maskHigh |= (1ULL << pin);
        else                         maskLow |= (1ULL << pin);
    }


    // No pin is ever armed at the level it is already at, by construction -
    // each one is armed at the opposite. So there is no immediate re-wake to
    // guard against and no pin has to be excluded to prevent one.
    uint64_t mask = maskHigh | maskLow;


    DEBUG_LOG_PRINT(
        "[EspNowNode] Deep sleep for ");

    DEBUG_LOG_PRINT(seconds);

    DEBUG_LOG_PRINT(" s (");

    DEBUG_LOG_PRINT(delivered ? "normal" : "recovery");

    DEBUG_LOG_PRINT(") [");

    for (int i = 0; i < _wakePinCount; i++)
    {
        uint8_t pin = _wakePins[i];

        if (i > 0) DEBUG_LOG_PRINT(" ");

        DEBUG_LOG_PRINT("gpio");

        DEBUG_LOG_PRINT(pin);

        DEBUG_LOG_PRINT(digitalRead(pin) == LOW ? "=LOW->wakeHIGH" : "=HIGH->wakeLOW");
    }

    DEBUG_LOG("]");


#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
    if (mask != 0)
    {
        // --------------------------------------------------
        // Hold ONLY the pads the SDK would get wrong
        // --------------------------------------------------
        //
        // esp_deep_sleep_start() sets each wake pad's resistor from that pin's
        // WAKE MODE: pull-down for a pin armed wake-on-HIGH, pull-up for one
        // armed wake-on-LOW. What the pin actually needs is decided by its
        // WIRING, which is what HsWakePull records.
        //
        // Half the time those two agree, and when they agree the SDK is doing
        // exactly the right thing on its own:
        //
        //   wiring pull-down, armed HIGH  -> SDK pull-down   AGREE
        //   wiring pull-down, armed LOW   -> SDK pull-up     disagree
        //   wiring pull-up,   armed LOW   -> SDK pull-up     AGREE
        //   wiring pull-up,   armed HIGH  -> SDK pull-down   disagree
        //
        // Only the disagreeing pins need gpio_hold_en() to stop the SDK
        // overwriting them, and holding a pad is not free: it is an extra
        // mechanism between the sensor and the wake comparator, and on this
        // part it needs the deep-sleep variant as well. Holding a pad the SDK
        // was about to configure correctly adds risk and buys nothing.
        //
        // For a tank resting EMPTY - both floats low, both armed HIGH, both
        // wired pull-down - this means NO pad is held at all and the sleep
        // uses the plain, ordinary configuration the SDK is designed around.
        int heldCount = 0;

        for (int i = 0; i < _wakePinCount; i++)
        {
            uint8_t pin = _wakePins[i];

            bool armedHigh = (maskHigh & (1ULL << pin)) != 0;

            // What esp_deep_sleep_start() is going to apply by itself.
            bool sdkGivesPullDown = armedHigh;

            bool wantsPullDown = (_wakePull[i] == HS_WAKE_PULL_DOWN);


            // Set it regardless: correct before the sleep either way, and it
            // is what the pin reads on if anything looks before we sleep.
            if (wantsPullDown)
            {
                gpio_pulldown_en((gpio_num_t)pin);

                gpio_pullup_dis((gpio_num_t)pin);
            }
            else
            {
                gpio_pullup_en((gpio_num_t)pin);

                gpio_pulldown_dis((gpio_num_t)pin);
            }


            if (sdkGivesPullDown == wantsPullDown)
            {
                // The SDK agrees. Leave the pad alone and let it do its job.
                continue;
            }


            gpio_hold_en((gpio_num_t)pin);

            heldCount++;
        }


        if (heldCount > 0)
        {
            gpio_deep_sleep_hold_en();
        }


        DEBUG_VALUE("[EspNowNode] pads held against the SDK's own pull choice", heldCount);


        // Step 1: the public wrapper, per group. This is what sets the
        // sleep's GPIO wakeup TRIGGER; the per-pin primitive below does not.
        // Whichever group is non-empty is enough to enable it.
        esp_err_t gpioResult = ESP_OK;

        if (maskHigh != 0)
        {
            gpioResult =
                esp_deep_sleep_enable_gpio_wakeup(
                    maskHigh,
                    ESP_GPIO_WAKEUP_GPIO_HIGH);
        }

        if (gpioResult == ESP_OK && maskLow != 0)
        {
            gpioResult =
                esp_deep_sleep_enable_gpio_wakeup(
                    maskLow,
                    ESP_GPIO_WAKEUP_GPIO_LOW);
        }

        if (gpioResult != ESP_OK)
        {
            DEBUG_VALUE("[EspNowNode] WARNING: GPIO wake could not be armed", esp_err_to_name(gpioResult));
        }


        // Step 2: assert each pin's own level, individually.
        //
        // Belt and braces, deliberately. If the two wrapper calls above are
        // additive then this changes nothing; if the second one overwrote the
        // first's per-pin type, this puts it back. Either way every pin ends
        // up waiting for its own edge, which is the property the tank needs
        // and the thing that cannot be left to chance.
        for (int i = 0; i < _wakePinCount; i++)
        {
            uint8_t pin = _wakePins[i];

            gpio_deep_sleep_wakeup_enable(
                (gpio_num_t)pin,
                (maskHigh & (1ULL << pin))
                    ? GPIO_INTR_HIGH_LEVEL
                    : GPIO_INTR_LOW_LEVEL);
        }
    }
#else
    // Said loudly rather than compiled away in silence. On a part without
    // deep-sleep GPIO wake this node still reports on its timer, but a float
    // moving will not wake it - which is a different product, and somebody
    // has to know.
    if (mask != 0)
    {
        DEBUG_LOG(
            "[EspNowNode] WARNING: this chip has no deep-sleep GPIO wake. "
            "Sensor inputs will NOT wake this node; only the RTC timer will.");
    }
#endif


    // --------------------------------------------------
    // Did anything change while we were arming?
    // --------------------------------------------------
    //
    // The window between reading a pin for the report and arming it for sleep
    // is milliseconds, but a door caught inside it would be armed for a
    // transition that had ALREADY happened - and the node would sleep through
    // the real one and report a stale state until its next timer wake.
    //
    // Returning without sleeping hands control back to the sketch's loop(),
    // which re-reads the sensor and calls reportAndSleep() again. The radio is
    // still up at this point, deliberately, so that costs one more report and
    // nothing else. The holds are released first, or the re-read would see a
    // frozen pad.
    for (int i = 0; i < _wakePinCount; i++)
    {
        uint8_t pin = _wakePins[i];

        bool armedForHigh = (maskHigh & (1ULL << pin)) != 0;

        bool nowLow = digitalRead(pin) == LOW;

        // A pin has reached its own wake level when it was armed for HIGH and
        // is now HIGH, or armed for LOW and is now LOW. Per pin, because the
        // levels are per pin.
        if (armedForHigh == nowLow) continue;


        DEBUG_LOG_PRINT(
            "[EspNowNode] gpio");

        DEBUG_LOG_PRINT(pin);

        DEBUG_LOG(
            " reached its wake level while arming. Reporting again instead of "
            "sleeping.");

        ReleaseWakePinHolds();

        return;
    }


    // The radio down before the chip goes down. esp_deep_sleep_start() would
    // do it anyway, but doing it here is what makes the last serial line
    // truthful about the order.
    if (_radioStarted)
    {
        esp_now_deinit();

        _radioStarted = false;
    }

    WiFi.mode(WIFI_OFF);


    Serial.flush();

    esp_deep_sleep_start();
}
