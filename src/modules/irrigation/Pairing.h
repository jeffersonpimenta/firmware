#pragma once
#include "modules/irrigation/IrrigationProtocol.h"
#include <stdint.h>

// Máquinas de estado do pareamento (spec §6). Puras: quem envia rádio é o módulo.
class StationPairing
{
  public:
    enum class State : uint8_t { IDLE, WINDOW, COMMITTED };
    static constexpr uint32_t WINDOW_MS = 2 * 60 * 1000, ANNOUNCE_INTERVAL_MS = 10 * 1000;

    void openWindow(uint32_t nowMs);
    void tick(uint32_t nowMs);
    bool announceDue(uint32_t nowMs);
    bool onGrant(const IrrigationProto::PairGrant &g, uint32_t nowMs);
    State state() const { return st; }
    const IrrigationProto::PairGrant &grant() const { return granted; }

  private:
    State st = State::IDLE;
    uint32_t windowStartMs = 0;
    uint32_t lastAnnounceMs = 0;
    bool announced = false;
    IrrigationProto::PairGrant granted = {};
};

class GatewayPairing
{
  public:
    static constexpr uint32_t WINDOW_MS = 2 * 60 * 1000, REGRANT_COOLDOWN_MS = 5 * 1000;

    void openWindow(uint32_t nowMs);
    void tick(uint32_t nowMs);
    bool windowOpen() const { return open; }
    bool approveAnnounce(uint32_t nodeId, uint32_t nowMs);

  private:
    bool open = false;
    uint32_t windowStartMs = 0;
    struct Recent {
        uint32_t node = 0;
        uint32_t atMs = 0;
    };
    Recent recent[8];
};
