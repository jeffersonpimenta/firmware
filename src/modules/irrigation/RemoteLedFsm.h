#pragma once
#include <stdint.h>

// FSM do LED de feedback da botoeira (no nó da botoeira). Uma instância por slot de LED.
// IDLE/OFF → (press) BLINK → (push ligado) SOLID | (push desligado / timeout) OFF.
class RemoteLedFsm {
  public:
    enum class State : uint8_t { OFF, BLINK, SOLID };
    // > REMOTE_FALLBACK_DEFAULT_MS (5 s): mantém o pisca durante a janela de fallback P2P
    // + o ida-e-volta do ACK direto, evitando apagar-e-reacender no instante do disparo.
    static constexpr uint32_t BLINK_TIMEOUT_MS = 7000;
    static constexpr uint32_t BLINK_PERIOD_MS = 250;

    void onPress(uint32_t nowMs) { st = State::BLINK; pressMs = nowMs; }
    void onLedState(bool on, uint32_t) { st = on ? State::SOLID : State::OFF; }

    State state(uint32_t nowMs)
    {
        if (st == State::BLINK && (uint32_t)(nowMs - pressMs) > BLINK_TIMEOUT_MS)
            st = State::OFF;
        return st;
    }

    bool ledOn(uint32_t nowMs)
    {
        switch (state(nowMs)) {
        case State::SOLID: return true;
        case State::BLINK: return ((nowMs / BLINK_PERIOD_MS) & 1u) != 0;
        default: return false;
        }
    }

  private:
    State st = State::OFF;
    uint32_t pressMs = 0;
};
