#pragma once
#include <stdint.h>

// Detector de gestos do botão multifunção (spec §8.6). Alimentar com amostras
// periódicas (~25 ms); eventos disparam no momento correto da linha do tempo.
class ButtonGestureDetector
{
  public:
    enum class Event : uint8_t { NONE, SHORT, DOUBLE, LONG_3S, HOLD_10S };
    static constexpr uint32_t DEBOUNCE_MS = 30, DOUBLE_GAP_MS = 400, SHORT_MAX_MS = 1000;
    static constexpr uint32_t LONG_MS = 3000, HOLD_MS = 10000;

    Event update(bool pressed, uint32_t nowMs);

  private:
    bool stable = false;       // estado debounced
    bool rawLast = false;
    uint32_t rawSinceMs = 0;   // desde quando o estado cru está estável
    uint32_t pressStartMs = 0;
    bool longFired = false;
    bool holdFired = false;
    bool shortArmed = false;   // soltou <1 s; aguardando gap de dupla
    uint32_t shortArmedMs = 0;
    bool doubleDetected = false;  // DOUBLE foi detectado nesta pressão
};
