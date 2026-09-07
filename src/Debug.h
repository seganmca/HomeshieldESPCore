#pragma once

#include <Arduino.h>

// ============================================================
// DEBUG CONFIGURATION
// ============================================================

// Enable / disable debug logging here.
//
// 1 = enabled
// 0 = disabled
//
#define DEBUG 0


#if DEBUG

// ============================================================
// Basic Logging
// ============================================================

// Print without newline
#define DEBUG_LOG_PRINT(x) \
    Serial.print(x)

// Print with newline
#define DEBUG_LOG(x) \
    Serial.println(x)

// Formatted print
#define DEBUG_LOG_PRINTF(...) \
    Serial.printf(__VA_ARGS__)

// Print label + value
#define DEBUG_VALUE(label, value)       \
    do                                  \
    {                                   \
        Serial.print("[DEBUG] ");       \
        Serial.print(label);            \
        Serial.print(": ");             \
        Serial.println(value);          \
    } while (0)


#else

// ============================================================
// Debug Disabled
// ============================================================

#define DEBUG_LOG_PRINT(x)

#define DEBUG_LOG(x)

#define DEBUG_LOG_PRINTF(...)

#define DEBUG_VALUE(label, value)

#endif