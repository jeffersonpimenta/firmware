#pragma once
#include <stdint.h>

// OFF (não DISABLED): DISABLED é macro do esp32-hal-gpio.h (modo de interrupção) e colidiria.
enum class InputMode : uint8_t { OFF = 0, FOLLOW, EDGE_BOTH, EDGE_TOGGLE, EDGE_OPEN, EDGE_CLOSE };

// Avaliador puro de UMA entrada digital. Aplica polaridade, debounce (dependente
// do modo), detecção de nível/borda e renovação de 60 s (FOLLOW). Fonte única de
// acionamento por GPIO. Testável sem GPIO.
class InputActuationPolicy
{
  public:
    enum class Intent : uint8_t { NONE, OPEN, CLOSE, TOGGLE };
    static constexpr uint32_t DEBOUNCE_EDGE_MS = 50;
    static constexpr uint32_t DEBOUNCE_LEVEL_MS = 100;
    static constexpr uint32_t RENEW_MS = 60000;

    void configure(InputMode mode, bool activeLow);
    Intent update(bool rawHigh, uint32_t nowMs);
    bool active() const { return _stable; }
    InputMode mode() const { return _mode; }

  private:
    // Returns the debounce threshold for the current mode.
    // update() returns NONE immediately for DISABLED, so that path never reads this.
    uint32_t debounceMs() const { return _mode == InputMode::FOLLOW ? DEBOUNCE_LEVEL_MS : DEBOUNCE_EDGE_MS; }
    InputMode _mode = InputMode::OFF;
    bool _activeLow = false;
    bool _lastRaw = false; // último "pressed" visto (pós-polaridade)
    uint32_t _sinceMs = 0; // quando o raw mudou
    bool _stable = false;  // pós-debounce
    bool _armed = true;    // p/ EDGE_TOGGLE/OPEN/CLOSE: rearma ao soltar
    uint32_t _lastRenewMs = 0;
};
