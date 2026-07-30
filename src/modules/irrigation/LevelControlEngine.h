#pragma once
#include "modules/irrigation/LevelControlTable.h"
#include <stddef.h>
#include <stdint.h>

struct LevelInput {
    bool present = false;     // existe snapshot p/ (sensorNode, sensorIdx)
    bool active = false;      // estado digital corrente
    bool fresh = false;       // atMs do heartbeat dentro de staleTimeoutS
};

struct LevelIntent {
    uint8_t ruleId = 0;
    uint8_t zoneId = 0;
    enum class Act : uint8_t { NONE, START, RENEW, STOP, STALE_STOP } act = Act::NONE;
    uint16_t durS = 0;        // START/RENEW
};

class LevelControlEngine {
  public:
    static constexpr uint32_t RENEW_INTERVAL_MS = 60000;
    static constexpr uint16_t OPEN_CEILING_S = 7200; // == HydraulicGroupEngine::PUMP_CEILING_S
    // inputs[i] corresponde a tbl.ruleAt(i). Escreve até maxOut intents (1 por regra). Devolve a contagem.
    size_t evaluate(const LevelControlTable &tbl, const LevelInput *inputs, size_t nInputs, uint32_t nowMs,
                    LevelIntent *out, size_t maxOut);

  private:
    struct Rt {
        uint8_t id = 0;
        bool on = false;
        bool staleAlerted = false;
        uint32_t lastOnMs = 0, lastOffMs = 0, lastRenewMs = 0;
    };
    Rt rt[LevelControlTable::MAX];
    Rt &rtFor(uint8_t id);
};
