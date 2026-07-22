#pragma once
#include "modules/irrigation/InterlockEngine.h" // enums
#include <stddef.h>
#include <stdint.h>

struct InterlockRule {
    uint8_t id = 0;             // 0 = slot vazio
    uint8_t tipo = 0;           // InterlockTipo
    uint32_t node = 0;          // SENSOR: estação dona
    uint8_t sensorIdx = 0;      // 0..3
    uint8_t condicao = 0;       // InterlockCond
    int32_t valorCenti = 0;
    uint16_t histereseCenti = 0;
    uint8_t acao = 0;           // InterlockAcao
    uint8_t zoneIds[8] = {0};   // 0 = fim da lista
    bool todas = false;         // "*"
    char mensagem[24] = {0};
    uint8_t maxAbertas = 0;     // SIMULTANEIDADE
};

class InterlockTable {
  public:
    static constexpr size_t MAX = 16;
    static constexpr uint32_t MAGIC = 0x494C4B31; // "ILK1"
    bool upsert(const InterlockRule &r);   // por id (1..255); false = cheia
    bool removeById(uint8_t id);
    const InterlockRule *byId(uint8_t id) const;
    const InterlockRule *ruleAt(size_t index) const; // index-ésima ocupada
    // Acesso por slot físico (0..MAX-1), não por ocupação. nullptr se slot vazio.
    const InterlockRule *ruleAtSlot(size_t slot) const;
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // falha => tabela vazia

  private:
    InterlockRule rules[MAX];
};
