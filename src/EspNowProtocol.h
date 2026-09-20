#pragma once

#include <Arduino.h>

// ============================================================
// ESP-NOW wire protocol (milestone 38)
// ============================================================
//
// ONE declaration, included by both halves of the conversation -
// EspNowHub on the ESP32-S3 and EspNowNode on the ESP32-C3. The
// M36 proof of concept kept a copy of its struct in each sketch
// and said so; two declarations of one wire format drift in
// silence, and this is the file that stops that happening.
//
// M38 carried FOUR messages, which onboard a sensor node to a
// hub and do nothing else: no command, no state, no event, no
// heartbeat. Milestone 40 adds TWO more, 5 and 6, in the space
// M38 left open and behind the same header and the same
// validation. They ask and answer one question - does the hub a
// node stored still have that node - and they carry no state,
// no command and no telemetry either.
//
// Milestone 41 adds 7 and 8, which finally do carry telemetry:
// one sensor reading from an adopted node, and the hub's
// acknowledgement of it. Still no command in this protocol and
// still nothing a hub can tell a node to do - a node wakes, says
// what it reads, and goes back to sleep.
//
// Every frame is a packed struct, not JSON. A node has no JSON
// reader, the fields are all fixed width, and the whole exchange
// has to fit inside ESP-NOW's 250-byte payload - which the
// largest frame here does, at 92 bytes.
// ============================================================

// The milestone number, as the POC used 0x36 for its own.
static const uint8_t HS_ESPNOW_MAGIC = 0x38;

static const uint8_t HS_ESPNOW_VERSION = 1;


enum : uint8_t
{
    MSG_DISCOVERY_REQUEST  = 1,   // hub  -> broadcast
    MSG_DISCOVERY_RESPONSE = 2,   // node -> hub
    MSG_DISCOVERY_ACK      = 3,   // hub  -> node
    MSG_IDENTITY_CONFIRM   = 4,   // node -> hub

    // Milestone 40. A provisioned node asking whether the hub it
    // stored still has it, and the hub's answer. Added at 5 and 6
    // as M38 left room for: same header, same validator, same
    // one-slot inbox on both sides.
    MSG_RELATIONSHIP_CHECK  = 5,  // node -> hub
    MSG_RELATIONSHIP_STATUS = 6,  // hub  -> node

    // Milestone 41. A sensor node telling its hub what it just read, and the
    // hub saying it has it. Added at 7 and 8 in the space M38 left and M40
    // continued: same header, same validator, same one-slot inbox on both
    // sides. This is the FIRST pair in this protocol that carries telemetry,
    // and it is still the only thing they carry - no command, no
    // configuration, no clock.
    MSG_NODE_REPORT         = 7,  // node -> hub
    MSG_NODE_REPORT_ACK     = 8,  // hub  -> node
};


// --------------------------------------------------
// Why a node woke up (milestone 41)
// --------------------------------------------------
//
// DIAGNOSTIC on the wire: the hub records it and nothing branches on it. It
// exists because "the tank reported 100" and "the tank reported 100 because
// the float moved" are different events to somebody reading a log, and the
// node is the only thing that can tell them apart.
//
// WAKE_POWER_ON is not a deep-sleep wake at all - it is a cold boot or a
// reset - and it is the one value the NODE branches on locally: it is the
// only wake that runs M40's relationship check.
enum HsNodeWake : uint8_t
{
    WAKE_POWER_ON = 0,
    WAKE_SENSOR   = 1,   // a sensor input pulled the node awake
    WAKE_BUTTON   = 2,   // the dedicated reset / manual wake input
    WAKE_TIMER    = 3,   // the RTC timer, normal or recovery
};


// The hub's answer to a relationship check. There is no third
// value and there must not be: "I cannot tell" is expressed by
// not answering, and the node treats silence as "keep what you
// have" (M40 requirement 6).
enum HsRelationshipVerdict : uint8_t
{
    RELATIONSHIP_VALID   = 1,
    RELATIONSHIP_REVOKED = 2,
};


// Why a hub refused a node it had just heard from.
enum HsEspNowReason : uint8_t
{
    REASON_NONE            = 0,
    REASON_ALREADY_ADOPTED = 1,   // this MAC is already in the registry
    REASON_REGISTRY_FULL   = 2,   // MAX_NODES reached
    REASON_MALFORMED       = 3,   // metadata missing or not decodable
    REASON_WRONG_SESSION   = 4,
};


// --------------------------------------------------
// Header
// --------------------------------------------------
//
// Every frame starts with this, so a receiver can validate
// before it decodes. sessionId is random, non-zero and
// hub-generated per discovery: it is what makes a frame from a
// session that has since been cancelled droppable rather than
// mistakable for a live one.
//
// sequence is the retry counter within one session. It is
// DIAGNOSTIC: nothing branches on it, and it wraps.
typedef struct __attribute__((packed))
{
    uint8_t  magic;
    uint8_t  protocolVersion;
    uint8_t  msgType;
    uint8_t  sequence;
    uint32_t sessionId;
} HsEspNowHeader;                 // 8 bytes


// --------------------------------------------------
// Hub -> broadcast
// --------------------------------------------------
//
// Broadcast because an unprovisioned node has no hub to address
// and the hub cannot leave the router's channel to find it (see
// EspNowHub.h). targetMac is the whole of the addressing: a node
// answers ONLY when it matches its own factory MAC, which is
// what makes a room full of unprovisioned nodes safe.
typedef struct __attribute__((packed))
{
    HsEspNowHeader header;
    uint8_t        hubMac[6];
    uint8_t        targetMac[6];
} HsDiscoveryRequest;             // 20 bytes


// --------------------------------------------------
// Node -> hub
// --------------------------------------------------
//
// The node is the source of truth for its own Device metadata,
// so this is where DeviceType, DeviceKey and DefaultName enter
// HomeShield. hubMac is echoed so a reply intended for another
// hub is detectable rather than merely unlikely.
//
// Fixed-size char arrays rather than a length-prefixed encoding:
// all three strings already have hard upper bounds on the server
// (DeviceType <= 50, DeviceKey <= 32, DefaultName <= 100), and a
// fixed frame is one memcpy to validate.
typedef struct __attribute__((packed))
{
    HsEspNowHeader header;
    uint8_t        nodeMac[6];
    uint8_t        hubMac[6];
    char           deviceType[24];
    char           deviceKey[16];
    char           defaultName[32];
} HsDiscoveryResponse;            // 92 bytes


// --------------------------------------------------
// Hub -> node
// --------------------------------------------------
//
// nodeMac is echoed so the node can re-verify that this ACK is
// for it, not for whichever node the hub spoke to last.
typedef struct __attribute__((packed))
{
    HsEspNowHeader header;
    uint8_t        hubMac[6];
    uint8_t        nodeMac[6];
    uint8_t        accepted;      // 1 = adopt me, 0 = refused
    uint8_t        reason;        // HsEspNowReason when accepted == 0
} HsDiscoveryAck;                 // 22 bytes


// --------------------------------------------------
// Node -> hub
// --------------------------------------------------
//
// stored says whether the hub's MAC actually reached NVS. A node
// that could not write is NOT provisioned, and the hub must not
// register a child for it, so this flag is load bearing rather
// than informational.
typedef struct __attribute__((packed))
{
    HsEspNowHeader header;
    uint8_t        nodeMac[6];
    uint8_t        hubMac[6];
    uint8_t        stored;
} HsIdentityConfirm;              // 21 bytes


// --------------------------------------------------
// Node -> hub (milestone 40)
// --------------------------------------------------
//
// Sent by a PROVISIONED node at boot, addressed to the hub MAC it
// has in NVS. hubMac is carried as well as addressed to, so a hub
// that receives a frame sweeping past it on the air can tell
// "this node is asking ME" from "this node is asking somebody
// else" without trusting the destination address alone.
//
// sessionId is generated by the NODE here - the only message in
// this protocol where it is - because the node owns this
// exchange. The hub echoes it, which is what lets the node ignore
// an answer to a check it has already finished.
typedef struct __attribute__((packed))
{
    HsEspNowHeader header;
    uint8_t        nodeMac[6];
    uint8_t        hubMac[6];
} HsRelationshipCheck;            // 20 bytes


// --------------------------------------------------
// Hub -> node (milestone 40)
// --------------------------------------------------
//
// The hub's registry IS the answer: the node's MAC is in it, or
// it is not. No lookup at the Control Server, no MQTT round trip,
// nothing that could make this depend on the hub being online.
typedef struct __attribute__((packed))
{
    HsEspNowHeader header;
    uint8_t        hubMac[6];
    uint8_t        nodeMac[6];
    uint8_t        verdict;       // HsRelationshipVerdict
} HsRelationshipStatus;           // 21 bytes


// --------------------------------------------------
// Node -> hub (milestone 41)
// --------------------------------------------------
//
// One reading, sent on every wake by a node that has already been adopted.
// It is the whole of a sensor node's outbound traffic: there is no separate
// heartbeat frame, because a report IS the heartbeat - a node that has
// nothing new to say still wakes on its timer and says the same thing again,
// which is what lets the hub tell a quiet sensor from a dead one.
//
// sessionId is generated by the NODE, as in MSG_RELATIONSHIP_CHECK and for
// the same reason: the node owns this exchange and the hub echoes the id,
// which is what lets the node ignore an ACK for a report it has already
// given up on.
//
// state is the existing wire contract's integer, unchanged - 0/1 for a door,
// 0/50/100 for a tank. int16_t rather than uint8_t because the contract is
// signed on the server and a device type added later may want a negative
// reading; two bytes here costs nothing and a range that cannot express one
// would cost a migration.
//
// battery is carried but NOT yet consumed: battery is a MODULE fact in
// HomeShield (DeviceTelemetry) and a node is a Device, so there is nowhere
// correct to put it today. It is -1 from every node in this milestone.
typedef struct __attribute__((packed))
{
    HsEspNowHeader header;
    uint8_t        nodeMac[6];
    uint8_t        hubMac[6];
    int16_t        state;
    uint8_t        wakeReason;   // HsNodeWake
    int8_t         battery;      // -1 = not available, else 0..100
    uint8_t        recovering;   // 1 = the node is on the recovery schedule
} HsNodeReport;                  // 25 bytes


// --------------------------------------------------
// Hub -> node (milestone 41)
// --------------------------------------------------
//
// accepted == 1 means THE HUB OWNS THIS REPORT NOW. It does not mean the
// Control Server has it: the node is battery powered and cannot wait on
// Wi-Fi, MQTT and a server round trip, while the hub is mains powered and
// can retry. The hub publishes from its own loop and keeps the reading until
// it succeeds.
//
// accepted == 0 is refusal, and there is exactly one reason a hub refuses:
// the MAC is not in its registry. An un-adopted node must not be able to
// create a child Device by reporting, so this is the door being held shut
// rather than an error path.
//
// nodeMac is echoed so the node can re-verify this ACK is for it.
typedef struct __attribute__((packed))
{
    HsEspNowHeader header;
    uint8_t        hubMac[6];
    uint8_t        nodeMac[6];
    uint8_t        accepted;     // 1 = recorded, 0 = refused
    uint8_t        reason;       // HsEspNowReason when accepted == 0
} HsNodeReportAck;               // 22 bytes


// --------------------------------------------------
// Shared helpers
// --------------------------------------------------

namespace HsEspNow
{
    inline bool ValidHeader(
        const uint8_t* data,
        int length,
        uint8_t expectedType,
        size_t expectedSize)
    {
        if (data == nullptr) return false;

        if (length != (int)expectedSize) return false;

        const HsEspNowHeader* header =
            reinterpret_cast<const HsEspNowHeader*>(data);

        if (header->magic != HS_ESPNOW_MAGIC) return false;

        if (header->protocolVersion != HS_ESPNOW_VERSION) return false;

        if (header->msgType != expectedType) return false;

        return true;
    }


    // "70AF0935923C" -> six bytes. False on anything else, so a
    // malformed target from the Control Server is refused rather
    // than silently becoming 00:00:00:00:00:00.
    inline bool ParseMac(
        const String& text,
        uint8_t* out)
    {
        if (text.length() != 12) return false;

        for (int i = 0; i < 6; i++)
        {
            int value = 0;

            for (int half = 0; half < 2; half++)
            {
                char c = text[i * 2 + half];

                int digit;

                if (c >= '0' && c <= '9')      digit = c - '0';
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else return false;

                value = (value << 4) | digit;
            }

            out[i] = (uint8_t)value;
        }

        return true;
    }


    // Six bytes -> "70AF0935923C". The form the QR code carries
    // and the form Module.HardwareId is stored in.
    inline String FormatMac(
        const uint8_t* mac)
    {
        char text[13];

        snprintf(
            text,
            sizeof(text),
            "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        return String(text);
    }


    // Copies into a fixed field and guarantees the NUL. A value
    // too long is truncated rather than overrunning: the caller's
    // bounds are the server's, which are larger than these.
    inline void CopyField(
        char* destination,
        size_t size,
        const String& value)
    {
        memset(destination, 0, size);

        size_t length = value.length();

        if (length > size - 1) length = size - 1;

        memcpy(destination, value.c_str(), length);
    }


    // A fixed field is only usable if it is NUL-terminated inside
    // its own bounds AND not empty.
    inline bool ReadField(
        const char* field,
        size_t size,
        String& out)
    {
        for (size_t i = 0; i < size; i++)
        {
            if (field[i] == '\0')
            {
                out = String(field);

                return out.length() > 0;
            }
        }

        out = "";

        return false;
    }
}
