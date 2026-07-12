#pragma once

#include <cstdint>
#include <cstring>

class MirrorMode
{
  public:
    static constexpr uint16_t OPEN_S = 120;     // duração de cada comando (fail-safe ≤120 s)
    static constexpr uint32_t RENEW_MS = 60000; // renovação (§4.2) enquanto entrada ativa
    static constexpr uint32_t DEBOUNCE_MS = 100;
    static constexpr size_t INPUTS = 4;
    static constexpr uint32_t MAGIC = 0x494D5231; // "IMR1"

    struct Action {
        enum class T : uint8_t { NONE, OPEN, CLOSE } t = T::NONE;
        uint8_t input = 0; // qual entrada causou (zona = glue via ZoneTable::byFonte)
    };

    MirrorMode();

    void setEnabled(bool e);
    bool enabled() const;
    // bitmap já com polaridade aplicada (bit i = entrada i ativa). 1 ação por
    // chamada; chamar até NONE. OPEN repete a cada RENEW_MS enquanto ativa.
    Action update(uint8_t inputsBitmap, uint32_t nowMs);
    bool inputActive(uint8_t input) const; // pós-debounce (p/ bypass do cronograma)
    size_t serialize(uint8_t *buf, size_t cap) const;   // só o flag enabled
    bool deserialize(const uint8_t *buf, size_t n);

  private:
    struct InputState {
        uint8_t rawLast = 0;         // último bitmap visto para este input
        uint32_t rawSinceMs = 0;     // quando vimos a mudança
        bool stable = false;         // pós-debounce
        uint32_t lastRenewMs = 0;    // quando renovamos a abertura
        bool needClose = false;      // drain flag (desabilitação)
    };

    bool _enabled = false;
    InputState _inputs[INPUTS] = {};
};
