#pragma once
#include <stdint.h>
#include "IrrigationSettings.h" // MAX_DIGITAL_IN

// Rastreia, por entrada-botão, um gatilho pendente que vira comando direto (P2P
// fallback) se o gateway não confirmar (REMOTE_LED) dentro de windowMs. Puro.
class RemoteFallbackTracker {
  public:
    explicit RemoteFallbackTracker(uint32_t windowMs) : window(windowMs) {}
    void setWindow(uint32_t windowMs) { window = windowMs; }

    void arm(uint8_t idx, uint32_t nowMs)
    {
        if (idx >= IrrigationSettings::MAX_DIGITAL_IN)
            return;
        slots[idx].armed = true;
        slots[idx].dueMs = nowMs + window;
    }
    void clear(uint8_t idx)
    {
        if (idx < IrrigationSettings::MAX_DIGITAL_IN)
            slots[idx].armed = false;
    }
    // Preenche out[] com índices expirados (dispara 1×), limpando-os. Retorna a contagem.
    size_t takeExpired(uint32_t nowMs, uint8_t *out, size_t cap)
    {
        size_t k = 0;
        for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
            if (slots[i].armed && (int32_t)(nowMs - slots[i].dueMs) >= 0) {
                slots[i].armed = false;
                if (k < cap)
                    out[k++] = i;
            }
        }
        return k;
    }

  private:
    struct Slot {
        bool armed = false;
        uint32_t dueMs = 0;
    };
    uint32_t window;
    Slot slots[IrrigationSettings::MAX_DIGITAL_IN];
};
