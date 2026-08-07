#pragma once
#include <stddef.h>
#include <stdint.h>

// Regra de supressão climática (ABI on-disk: struct memcpy'd inteiro).
// Ordem dos campos escolhida para não gerar padding (align máximo = 2).
struct WeatherRule {
    static constexpr uint8_t MAX_ZONE_TARGETS = 16;
    static constexpr uint8_t MAX_GROUP_TARGETS = 8;
    static constexpr uint8_t NOME_LEN = 32;
    static constexpr uint8_t MSG_LEN = 48;

    uint16_t limiarMmCenti = 0;                 // chuva prevista 12h (centi-mm)
    uint8_t  id = 0;                            // 0 = slot vazio
    uint8_t  enabled = 1;
    uint8_t  limiarPct = 0;                     // probabilidade de chuva (%)
    uint8_t  zonaIds[MAX_ZONE_TARGETS] = {0};   // 0 = fim/vazio
    uint8_t  grupoIds[MAX_GROUP_TARGETS] = {0}; // 0 = fim/vazio
    char     nome[NOME_LEN] = {0};
    char     mensagem[MSG_LEN] = {0};

    bool coversZone(uint8_t zoneId) const;
    bool coversGroup(uint8_t groupId) const;
};
static_assert(sizeof(WeatherRule) == 110, "WeatherRule é ABI on-disk; ajuste com cuidado");

class WeatherRuleTable {
  public:
    static constexpr size_t MAX = 8;
    static constexpr uint32_t MAGIC = 0x57524C31; // "WRL1"

    bool upsert(const WeatherRule &r);            // por id (1..255); false = cheia
    bool removeById(uint8_t id);
    const WeatherRule *byId(uint8_t id) const;
    const WeatherRule *ruleAt(size_t index) const; // index-ésima ocupada
    size_t count() const;
    uint8_t nextFreeId() const;                    // menor id livre 1..255 (0 = sem espaço)

    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // falha => tabela vazia

  private:
    WeatherRule rules[MAX];
};
