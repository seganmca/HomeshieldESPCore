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
// M38 carries FOUR messages and no more. They onboard a sensor
// node to a hub and they do nothing else: no command, no state,
// no event, no heartbeat. Those are milestone 39's, and the
// msgType space from 5 upwards is left open for them behind this
// same header and this same validation.
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
