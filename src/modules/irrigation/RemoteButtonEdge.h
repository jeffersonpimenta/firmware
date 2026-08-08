#pragma once
#include <stdint.h>

// Detector de borda de subida com debounce, para botoeiras (entradas digitais).
// raw = nível lógico JÁ com polaridade aplicada (true = pressionado). update() retorna
// true UMA vez por borda de subida confirmada; rearma só após ler estável em baixo.
class RemoteButtonEdge {
  public:
    static constexpr uint32_t DEBOUNCE_MS = 50;
    bool update(bool raw, uint32_t nowMs)
    {
        if (raw != lastRaw) { lastRaw = raw; sinceMs = nowMs; }
        if ((uint32_t)(nowMs - sinceMs) >= DEBOUNCE_MS && raw != stable) {
            stable = raw;
            if (stable) { // borda de subida estável
                if (armed) { armed = false; return true; }
            } else {
                armed = true; // soltou → rearma
            }
        }
        return false;
    }

  private:
    bool stable = false;
    bool lastRaw = false;
    uint32_t sinceMs = 0;
    bool armed = true;
};

// Resolução do toggle: dada a saída atual, devolve a ação (1 = abrir, 0 = fechar).
inline uint8_t remoteToggleAction(bool currentlyOn) { return currentlyOn ? 0u : 1u; }
