#pragma once
#include <stdint.h>

// Tabela ÚNICA dos padrões de LED (spec §8.7). ledOn é função pura do tempo:
// sem estado além do modo — fácil de revisar e de testar.
class LedPatternController
{
  public:
    enum class Mode : uint8_t { NORMAL, NO_GATEWAY, CONFIG_PENDING, PAIRING, OUTPUT_OPEN, BATTERY_SOS };
    void setMode(Mode m) { mode = m; }
    Mode currentMode() const { return mode; }
    bool ledOn(uint32_t nowMs) const;

  private:
    Mode mode = Mode::NORMAL;
};
