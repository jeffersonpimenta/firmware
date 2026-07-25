#pragma once
#include "modules/irrigation/ServiceBackup.h"
#include "modules/irrigation/ServiceVault.h"
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

// Owns the vault + active scan; produces intents the module executes on hardware.
// Hardware-free → native-testable end to end.
class ServiceController {
  public:
    explicit ServiceController(IProfileStore &store) : vault(store) {}
    ServiceVault &getVault() { return vault; }

    // Retune plan for a client (§11.3). false = no such client / bad psk.
    bool planRetune(const char *id, RetunePlan &out);

    // Active-client scan (§11.4).
    void onSurveyReply(const ScanEntry &e) { scan.add(e); }
    const ScanResults &scanResults() const { return scan; }
    void clearScan() { scan.clear(); }

    // Next outgoing seq for (client,node); needResync=true when the counter is unknown (§11.5).
    uint32_t nextSeq(const char *id, uint32_t node, bool &needResync);
    // Consume a RESYNC_SEQ REPLY: resume at lastSeq+1.
    void onResyncReply(const char *id, uint32_t node, uint32_t lastSeq);

    // Anexa uma linha ao servico.jsonl (§11.8 aba Log). up=uptime; gwTs=timestamp adotado do gateway (0=sem RTC).
    void logService(const char *ev, uint32_t node, uint32_t gwTs);

  private:
    ServiceVault vault;
    ScanResults scan;
};
