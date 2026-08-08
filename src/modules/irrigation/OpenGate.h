#pragma once
#include <stddef.h>
#include <stdint.h>

// Contagem global de saídas abertas + fila FIFO de aberturas pendentes.
// A fila carrega {zoneId, durationS} para o glue reabrir com a duração do passo
// que enfileirou. Não sabe de intertravamentos: o glue combina o veredito do
// InterlockEngine (zona bloqueada) com a decisão de capacidade daqui.
class OpenGate {
  public:
    static constexpr size_t MAX_OPEN = 24; // = ZoneTable::MAX
    static constexpr size_t MAX_QUEUE = 24;
    enum class Decision : uint8_t { ADMIT, HOLD };
    struct Pending { uint8_t zoneId = 0; uint16_t durationS = 0; }; // zoneId 0 = nada

    void setCap(uint8_t cap) { capOpen = cap; } // 0 = sem limite
    Decision request(uint8_t zoneId, uint16_t durationS); // registra abertura; enfileira se cheia
    void release(uint8_t zoneId);      // zona fechou
    Pending nextAdmittable();          // {0,0} = nada a admitir agora; senão desenfileira 1
    size_t openCount() const;
    bool isQueued(uint8_t zoneId) const;
    bool isOpen(uint8_t zoneId) const; // true se a zona está registrada como aberta

  private:
    uint8_t open[MAX_OPEN] = {0};
    Pending queue[MAX_QUEUE] = {};
    size_t qHead = 0, qTail = 0, qSize = 0;
    uint8_t capOpen = 0;
    bool hasCapacity() const { return capOpen == 0 || openCount() < capOpen; }
    bool markOpen(uint8_t zoneId);
};
