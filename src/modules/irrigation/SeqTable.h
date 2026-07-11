#pragma once
#include <stddef.h>
#include <stdint.h>

// Anti-replay (spec §4.2): last_seq por remetente. Tabela fixa em RAM;
// persistência em NVS entra na Fase 2 do plano.
class SeqTable
{
  public:
    static constexpr size_t MAX_PEERS = 8;

    // true = seq nova (aceita e registrada); false = replay/antiga (descartar).
    bool checkAndUpdate(uint32_t sender, uint32_t seq);
    // 0 se o remetente é desconhecido.
    uint32_t lastSeq(uint32_t sender) const;

  private:
    struct Entry {
        uint32_t node = 0;
        uint32_t seq = 0;
    };
    Entry entries[MAX_PEERS];
};
