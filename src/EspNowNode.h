#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "EspNowProtocol.h"

// ============================================================
// EspNowNode - the ESP32-C3 half of onboarding (milestone 38)
// ============================================================
//
// A sensor node is a DEVICE hosted by a Sensor Hub's Module. It
// is not a Module, it never joins Wi-Fi, it never registers with
// the Control Server and it never speaks MQTT. Its entire
// existence in HomeShield is a Device row the hub creates on its
// behalf.
//
// So this class does exactly one thing, and a node sketch does
// nothing else:
//
//   UNPROVISIONED  sweep the channels, listen for a discovery
//                  request naming THIS node's factory MAC,
//                  answer it, confirm the hub, persist the hub's
//                  MAC, restart
//   PROVISIONED    nothing at all
//
// M38 deliberately implements no sensor behaviour. No reed
// switch, no GPIO, no state, no events, no heartbeat. That is
// milestone 39's, and a node that did any of it before it had
// been adopted would be reporting to nobody.
//
// ------------------------------------------------------------
// Channels: why the node sweeps and the hub does not
// ------------------------------------------------------------
//
// ESP-NOW only reaches a peer on the same channel. The hub is
// joined to the household router, so its channel is the router's
// and it cannot leave without dropping Wi-Fi, MQTT and the
// Control Server. An unprovisioned node has never seen that
// router and cannot know the channel. The M36 proof of concept
// hard-coded channel 11 on both sides, which works exactly until
// somebody's router moves.
//
// So the node sweeps and the hub stays put. The node steps
// through channels 1..13 dwelling 300 ms on each - one full
// sweep every 3.9 s - while the hub broadcasts every 500 ms on
// its own. Any single dwell overlaps several broadcasts, so the
// node hears one within at most two sweeps, and it then LOCKS to
// that channel for the rest of the exchange.
//
// A consequence worth stating: a node that is asleep or
// unpowered cannot be discovered. That is why the app says
// "Turn on the sensor node to continue" and why discovery has no
// timeout.
//
// ------------------------------------------------------------
// Nothing is written until the exchange has succeeded
// ------------------------------------------------------------
//
// A failure at any point leaves the node UNPROVISIONED with an
// empty NVS namespace, and it goes back to sweeping. The factory
// MAC is eFuse and is never written by anything here.
// ============================================================

// ============================================================
// Sensor node sleep and delivery policy (milestone 41)
// ============================================================
//
// Every number M41 makes configurable, in one struct, with the
// approved defaults already in it. A sketch that is happy with
// them does not call configureSleep() at all.
//
// The two intervals are the whole of the recovery behaviour: a
// node that got its ACK sleeps for normalWakeSeconds, a node that
// did not sleeps for recoveryWakeSeconds and keeps doing so until
// one arrives. There is no third schedule and no backoff - a
// battery node that kept lengthening its retry would take longer
// to recover the longer the hub had been away, which is exactly
// backwards.
struct EspNowNodeSleep
{
    // NormalWakeInterval. One hour.
    uint32_t normalWakeSeconds = 3600;

    // RecoveryWakeInterval. Five minutes, used ONLY after a failed
    // delivery and only until the next successful one.
    uint32_t recoveryWakeSeconds = 300;

    // Bounded ACK attempts per wake. After these the node SLEEPS;
    // it never stays awake retrying, which is the point.
    int maxAttempts = 3;

    // How long one attempt waits for the ACK. Short: the hub
    // answers from its receive path, and a node holding its radio
    // open is spending the battery this milestone exists to save.
    uint32_t retryIntervalMs = 400;

    // The dedicated reset / manual wake input. -1 = not wired.
    // Held for resetHoldMs at boot, it clears this node's stored
    // hub relationship and restarts it into discovery; tapped, it
    // is simply a manual report.
    int resetPin = -1;

    bool resetActiveLow = true;

    uint32_t resetHoldMs = 5000;
};


// --------------------------------------------------
// How a wake pin is wired (milestone 41)
// --------------------------------------------------
//
// Which resistor holds the pin at its IDLE level while the node
// sleeps. It follows the WIRING and never the wake level: a
// switch to ground idles HIGH and needs a pull-up whichever edge
// is being watched for, and a switch to supply idles LOW and
// needs a pull-down.
//
// It has to be said here because the SDK gets it wrong for us.
// esp_deep_sleep_start() sets the resistor from the WAKE MODE
// (pull-down for a wake-on-HIGH pin), which is right only if the
// sensor drives the line both ways. Ours do not: they are
// switches. So the pad is configured from this and then held,
// and the hold is what stops the SDK overwriting it.
enum HsWakePull : uint8_t
{
    // Switch to ground. Idles HIGH. Both tank floats as originally
    // wired, the reed switch, and every reset button.
    HS_WAKE_PULL_UP = 0,

    // Switch to supply, or a sensor that drives the line high.
    // Idles LOW; "active" is HIGH.
    HS_WAKE_PULL_DOWN = 1,
};


class EspNowNodeClass
{
public:

    // deviceType  a canonical key from DeviceTypes.h
    // deviceKey   this node's KIND key - "door". The hub composes
    //             the module-local address from it and the MAC; a
    //             node does not know or need that composed value.
    // defaultName the display name until the household sets one
    //
    // Provisioned: returns immediately having started no radio.
    // Otherwise: brings up ESP-NOW and starts sweeping.
    void begin(
        const String& deviceType,
        const String& deviceKey,
        const String& defaultName);


    void loop();


    // ==================================================
    // Milestone 41 - reporting and deep sleep
    // ==================================================

    // Policy. Call BEFORE begin(): the reset pin is sampled during
    // begin(), and the wake intervals are read when the node goes
    // back to sleep.
    void configureSleep(
        const EspNowNodeSleep& sleep);


    // --------------------------------------------------
    // Declare a GPIO that may wake this node
    // --------------------------------------------------
    //
    // Must be a deep-sleep-wake-capable GPIO on this part. On the
    // ESP32-C3 that is GPIO 0-5 and nothing else - soc_caps.h says
    // so outright:
    //
    //   SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK
    //       (0ULL | BIT0 | BIT1 | BIT2 | BIT3 | BIT4 | BIT5)
    //
    // Any other pin is REFUSED here, with a warning, rather than
    // accepted and silently unable to fire: esp_deep_sleep_enable_
    // gpio_wakeup() answers ESP_ERR_INVALID_ARG for it, and a
    // sensor that is read correctly on every wake but can never
    // CAUSE one is the hardest version of this fault to see from
    // the outside. A refused pin is still read normally while the
    // node is awake; it just cannot end a sleep.
    //
    // Wakes on CHANGE, in either direction. At each sleep the pin
    // is armed for the level it is not currently at, so a door
    // wakes the node both when it opens and when it closes. The
    // caller does not declare a polarity and must not assume one:
    // "active" and "interesting" are different properties, and a
    // sensor's two edges are usually both worth reporting.
    //
    // The C3's GPIO wake is level triggered, but the level is per
    // pin: EnterDeepSleep() arms each one individually with
    // gpio_deep_sleep_wakeup_enable(), so pins resting at opposite
    // levels are all armed in the same sleep and none is dropped.
    // A tank's full float can wait for a rising edge while its
    // empty float waits for a falling one.
    //
    // `pull` says how the pin is wired, and it is not optional
    // information: the SDK sets each pad's resistor from the WAKE
    // MODE inside esp_deep_sleep_start and would get a switch
    // backwards. EnterDeepSleep() applies this pull and then holds
    // the pad so the SDK cannot overwrite it.
    //
    // The pin does need a pull-up and a switch to ground, which is
    // how every input in this tree is wired - the pull-up is
    // enabled for the sleep so a released input reads HIGH rather
    // than floating.
    //
    // Call before begin(). Returns false when the table is full.
    bool addWakePin(
        uint8_t gpio,
        HsWakePull pull = HS_WAKE_PULL_UP);


    // --------------------------------------------------
    // The whole of an adopted node's working life
    // --------------------------------------------------
    //
    // Send `state` to the hub, wait for the ACK, and deep sleep.
    // DOES NOT RETURN: it ends in esp_deep_sleep_start(), and the
    // next thing that runs is setup() after the next wake.
    //
    // It returns immediately and without sleeping when this node
    // is not provisioned, or has not finished settling: an
    // unadopted node must stay awake and discoverable, and a
    // sleeping one cannot be found (M38's "Turn on the sensor node
    // to continue" depends on this).
    //
    // The sketch passes the reading rather than this class taking
    // it, because what a state MEANS is the sketch's - a tank's
    // three float positions are not something the framework should
    // know.
    void reportAndSleep(
        int state);


    // Why this boot happened. WAKE_POWER_ON on a cold boot or a
    // reset, which is the only wake that runs M40's relationship
    // check.
    HsNodeWake WakeReason() const;


    // Whether this node is on the 5-minute recovery schedule
    // because its last delivery was not acknowledged.
    bool IsRecovering() const;


    bool IsProvisioned() const;


    // The adopting hub's MAC, 12 uppercase hex. Empty unless
    // provisioned. Milestone 39 addresses the hub with it.
    const String& HubMac() const;


    // The node's own permanent identity, from eFuse.
    String NodeMac() const;


    // Called from the ESP-NOW receive callback, which runs on the
    // Wi-Fi task. It only records; loop() does the work.
    void OnFrameReceived(
        const uint8_t* mac,
        const uint8_t* data,
        int length);


    // Called from the ESP-NOW send callback, on the same task. A unicast
    // frame's acknowledgement arrives here and nowhere else.
    void OnFrameSent(
        bool ok);


private:

    enum class State
    {
        // Sweeping the channels, listening. The resting state of a
        // node nobody has adopted.
        Discovery,

        // A request naming this node arrived. Channel locked, hub
        // peer added, responses going out until one is answered.
        Responding,

        // The hub's MAC is in NVS and the confirmation has been
        // sent. Restarting shortly.
        Confirmed,

        // Milestone 40. Booted from NVS and checking, before it
        // settles, that the hub it stored still has it. Sweeps the
        // channels sending RELATIONSHIP_CHECK to that one hub, and
        // ends in Provisioned - whether the hub says VALID or says
        // nothing at all. Only an explicit REVOKED clears NVS.
        RelationshipCheck,

        // Booted from NVS, relationship settled. Does nothing,
        // forever.
        Provisioned,
    };


    void StartRadio();

    void StepChannel();

    void ReportStatus();

    void HandleRequest(
        const HsDiscoveryRequest& request);

    void HandleAck(
        const HsDiscoveryAck& ack);

    // Milestone 40.
    void HandleRelationshipStatus(
        const HsRelationshipStatus& status);

    void SendRelationshipCheck();

    // Stops checking and settles as provisioned, keeping the stored
    // relationship. Used both for VALID and for "the hub never answered".
    void SettleProvisioned(
        const char* why);

    // Removes the stored hub relationship. Returns whether it could be
    // verified as gone.
    bool ClearPersisted();

    void SendResponse();

    void SendConfirm(
        bool stored);


    // --------------------------------------------------
    // Milestone 41
    // --------------------------------------------------

    // Works out why this boot happened, and samples the reset pin.
    // Returns true when the reset was held and this node has been
    // unprovisioned (the caller must then stop: a restart follows).
    bool ClassifyWakeAndCheckReset();

    // One attempt: point the peer at `channel`, send the report,
    // and wait up to retryIntervalMs for a matching ACK.
    bool SendReportOnChannel(
        uint8_t channel,
        int state);

    // Up to maxAttempts attempts across the cached channel and,
    // if that fails, the whole sweep. True when acknowledged.
    bool DeliverReport(
        int state);

    void EnterDeepSleep(
        bool delivered);

    // Releases the pad holds EnterDeepSleep() applied. A held pad keeps its
    // frozen configuration across the deep-sleep reset and ignores writes
    // until it is released, so this must run before anything reads a wake
    // pin - otherwise every read after a GPIO wake returns the level the pad
    // was frozen at rather than the level the sensor is at.
    void ReleaseWakePinHolds();

    // The hub's channel, learned at adoption and re-learned by a
    // sweep. 0 = not known.
    uint8_t CachedChannel() const;

    void RememberChannel(
        uint8_t channel);

    bool AddHubPeer();

    void AbandonSession();

    bool Persist();

    bool Load();


    State _state =
        State::Discovery;


    // Declared by the sketch; this node is the source of truth for
    // all three and they travel verbatim in DISCOVERY_RESPONSE.
    String _deviceType;

    String _deviceKey;

    String _defaultName;


    uint8_t _nodeMac[6] = {0};

    uint8_t _hubMac[6] = {0};

    String _hubMacText;


    uint32_t _sessionId = 0;

    uint8_t _sequence = 0;

    bool _hubPeerAdded = false;


    // --------------------------------------------------
    // Channel sweep
    // --------------------------------------------------

    static constexpr uint8_t FirstChannel = 1;

    // 1-13 covers ETSI and India. A build for a 1-11 domain moves
    // this; listening is not transmitting, and the node only ever
    // transmits on the channel the hub was already using.
    static constexpr uint8_t LastChannel = 13;

    // --------------------------------------------------
    // The dwell MUST exceed the hub's broadcast interval
    // --------------------------------------------------
    //
    // Bring-up fix. It was 300 ms against a hub broadcasting every 500 ms,
    // which left a 200 ms window on every visit in which the node was on the
    // right channel and no frame was sent - so hearing the hub was a coin toss
    // per visit rather than a certainty, and a run of bad luck looked exactly
    // like a node that could not hear at all.
    //
    // At 600 ms against 500 ms, every visit to the hub's channel contains at
    // least one broadcast. A full sweep is 13 x 600 ms = 7.8 s worst case,
    // which is the honest cost of making discovery deterministic instead of
    // probabilistic.
    //
    // If the hub's BroadcastInterval changes, this must stay above it.
    static constexpr unsigned long ChannelDwell = 600;

    uint8_t _channel = FirstChannel;

    unsigned long _channelSteppedAt = 0;


    // --------------------------------------------------
    // Diagnostics
    // --------------------------------------------------
    //
    // "Sweeping channels 1-13" said only that the code had started, not that
    // anything was arriving - which is precisely the question that could not be
    // answered during bring-up. These count what the radio ACTUALLY hears.

    bool _radioStarted = false;

    uint32_t _sweepsCompleted = 0;

    // Every ESP-NOW frame delivered to this node, from anyone, valid or not.
    uint32_t _framesHeard = 0;

    // Frames that were a HomeShield discovery request for some other node.
    uint32_t _framesForOthers = 0;

    // Frames this node put on the air, and how many of them went
    // unacknowledged. A DISCOVERY_RESPONSE is unicast, so a failure here means
    // the hub did not hear it.
    uint32_t _sendsCompleted = 0;

    uint32_t _sendsFailed = 0;

    unsigned long _lastStatusAt = 0;

    static constexpr unsigned long StatusInterval = 5000;


    // --------------------------------------------------
    // Retries
    // --------------------------------------------------

    static constexpr unsigned long ResponseInterval = 1000;

    static constexpr int MaxResponseAttempts = 5;

    unsigned long _lastResponseAt = 0;

    int _responseAttempts = 0;


    // The pause between confirming and restarting, so the frame is
    // actually on the air before the radio goes down.
    static constexpr unsigned long RestartDelay = 500;

    unsigned long _confirmedAt = 0;


    // --------------------------------------------------
    // Relationship check (milestone 40)
    // --------------------------------------------------
    //
    // Whether this node has a stored hub, as opposed to what state
    // the loop is in. IsProvisioned() answers with THIS, so a
    // sketch sees an adopted node as adopted throughout the check -
    // which it is. The check can only take the relationship away,
    // never grant one.
    bool _persisted = false;


    // --------------------------------------------------
    // Milestone 41 state
    // --------------------------------------------------

    EspNowNodeSleep _sleep;

    static constexpr int MaxWakePins = 4;

    uint8_t _wakePins[MaxWakePins] = {0};

    // Parallel to _wakePins by index. See HsWakePull.
    HsWakePull _wakePull[MaxWakePins] = {HS_WAKE_PULL_UP};

    int _wakePinCount = 0;

    HsNodeWake _wakeReason = WAKE_POWER_ON;

    // Which GPIOs actually caused the wake, latched by the hardware and read
    // before anything reconfigures a pad. BIT(n) set means GPIO n fired. Zero
    // for a timer or power-on wake.
    uint64_t _wakeGpioStatus = 0;

    // The channel the hub was last heard on. RAM copy of whichever
    // of the RTC and NVS caches answered; 0 when neither did.
    uint8_t _hubChannel = 0;

    // Set by the send callback so a failed transmission ends the
    // ACK wait immediately instead of burning retryIntervalMs on a
    // frame that provably never left.
    volatile bool _lastSendReported = false;

    volatile bool _lastSendOk = false;

    // Set while DeliverReport() is waiting, and cleared by a
    // matching MSG_NODE_REPORT_ACK.
    bool _reportAcked = false;

    // The node sends one check per channel visit, so a full sweep
    // is one attempt on every channel the hub could be on. Two
    // sweeps - about 16 s - then it gives up and stays provisioned.
    //
    // Deliberately small. A hub that is powered off is the ordinary
    // case, not a fault, and a node that kept sweeping would burn
    // its battery to learn nothing. It asks again at the next boot.
    static constexpr uint32_t MaxCheckSweeps = 2;

    // The sweep count itself is _sweepsCompleted, which the discovery
    // sweep already maintains - one counter, one sweep, whichever
    // question is being asked on it.
    //
    // There is no retry beyond MaxCheckSweeps and no escalation:
    // silence NEVER unprovisions. M40 requirement 6.

    // The channel the last check was sent on, so the peer is
    // re-pointed only when the sweep has actually moved.
    uint8_t _peerChannel = 0;


    // --------------------------------------------------
    // Inbox
    // --------------------------------------------------
    //
    // The receive callback runs on the Wi-Fi task and must not
    // touch NVS or the radio. It copies one frame here; loop()
    // drains it. One slot is enough: the exchange is strictly
    // turn-taking and a frame arriving while the previous one is
    // unread is a duplicate retry, which is exactly what should be
    // dropped.
    static constexpr size_t MaxFrame = 128;

    portMUX_TYPE _inboxLock =
        portMUX_INITIALIZER_UNLOCKED;

    uint8_t _inbox[MaxFrame] = {0};

    volatile int _inboxLength = 0;

    volatile bool _hasInbox = false;


    Preferences _preferences;


    // --------------------------------------------------
    // NVS (namespace "hs_node")
    // --------------------------------------------------
    //
    // node_state is the COMMIT MARKER, written last and removed
    // first, exactly as milestone 37's prov_state is. A power cut
    // mid-write leaves an unprovisioned node rather than one that
    // believes it belongs to a hub whose MAC it did not finish
    // writing.
    //
    // The factory MAC is NOT here and never will be. It is eFuse,
    // read at every boot, and nothing HomeShield does can change it.

    static constexpr const char* Namespace = "hs_node";

    static constexpr const char* HubMacKey = "hub_mac";

    static constexpr const char* VersionKey = "node_ver";

    static constexpr const char* StateKey = "node_state";

    // Milestone 41. The channel the hub was on when this node was
    // adopted, so an hourly wake can transmit at once instead of
    // sweeping 1-13 with the radio up - up to 7.8 s, which is the
    // most expensive thing a battery node can do.
    //
    // It is a CACHE and never load bearing: a wrong value costs
    // one sweep and is then corrected, and a missing one costs a
    // sweep on the first wake. It is deliberately NOT part of the
    // commit marker's contract for that reason.
    static constexpr const char* HubChannelKey = "hub_ch";

    static constexpr int CurrentVersion = 1;
};


extern EspNowNodeClass EspNowNode;
