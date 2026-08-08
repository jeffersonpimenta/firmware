#pragma once
#include <stddef.h>
#include <stdint.h>

struct RemoteTriggerRef {
    uint32_t node = 0;      // 0 = vazio
    uint8_t inputIdx = 0;   // 0..3
    uint8_t ledSlot = 255;  // 0/1 = slot de LED no nó do gatilho; 255 = nenhum
};

struct RemoteAssoc {
    uint8_t id = 0;          // 0 = slot vazio
    uint8_t enabled = 1;     // 0 = desativada (mantida na tabela)
    uint8_t targetZoneId = 0; // saída-alvo = zona do gateway
    static constexpr size_t MAX_TRIGGERS = 4;
    RemoteTriggerRef triggers[MAX_TRIGGERS];
    uint8_t triggerCount() const;
};

class RemoteButtonTable {
  public:
    static constexpr size_t MAX = 8;
    static constexpr uint32_t MAGIC = 0x49524231; // "IRB1"
    bool upsert(const RemoteAssoc &a);   // por id (1..255); false = cheia
    bool removeById(uint8_t id);
    const RemoteAssoc *byId(uint8_t id) const;
    const RemoteAssoc *assocAt(size_t index) const; // index-ésima ocupada; nullptr se >= count()
    size_t count() const;
    // Preenche out[] com associações cujo gatilho == (node,inputIdx); retorna quantas.
    size_t findByTrigger(uint32_t node, uint8_t inputIdx, const RemoteAssoc **out, size_t outCap) const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // falha => tabela vazia

  private:
    RemoteAssoc assocs[MAX];
};
