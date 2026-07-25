#pragma once
#include "modules/irrigation/ServiceBackup.h"
#include <cstddef>
#include <cstdint>

// SERVICO runtime helpers. Hardware-free: pure decisions + intents the module executes.

// Plan for re-tuning the radio to a client's channel (§11.3). ok=false → bad psk / empty name.
struct RetunePlan {
    char name[13];  // ≤ 12 + NUL, clamped to Meshtastic channel name limit
    uint8_t psk[32];
    size_t pskLen;
    uint8_t preset; // ModemPreset enum value
    bool ok;
};
RetunePlan channelFromProfile(const IrrigationService::LightProfile &p);
