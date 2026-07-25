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

// Config r/w route (§11.6). VIA_GATEWAY → gateway bumps epoch (epochToWrite=0);
// DIRECT (gateway unreachable) → write station at currentEpoch+1 and record pending.
enum class ConfigRoute : uint8_t { VIA_GATEWAY, DIRECT };
struct RouteDecision {
    ConfigRoute route;
    uint32_t epochToWrite;
};
RouteDecision decideConfigRoute(bool gatewayReachable, uint32_t currentEpoch);

// RESYNC requester (§11.5).
inline bool needsResync(bool haveSeq) { return !haveSeq; }
inline uint32_t resumeSeqFrom(uint32_t lastSeq) { return lastSeq + 1; }

// Active-client scan results (§11.4). Deduped by node (latest read wins).
struct ScanEntry {
    uint32_t node = 0;
    uint8_t role = 0;
    uint32_t epoch = 0;
    uint16_t vbatCentiV = 0;
    uint16_t fwVersion = 0;
    int32_t lat = 0, lon = 0;
    int8_t snrQuarterDb = 0;
};
class ScanResults {
  public:
    static constexpr size_t MAX = 32;
    void clear() { n = 0; }
    size_t count() const { return n; }
    const ScanEntry *at(size_t i) const { return i < n ? &e[i] : nullptr; }
    void add(const ScanEntry &x); // dedupe by node (update in place)

  private:
    ScanEntry e[MAX];
    size_t n = 0;
};
