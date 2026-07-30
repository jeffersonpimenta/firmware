#pragma once
#include <stddef.h>
#include <stdint.h>

// Regra de controle de nível (§ enchimento por boia). Avaliada no gateway.
struct LevelRule {
    uint8_t  id = 0;                  // 0 = slot vazio
    uint32_t sensorNode = 0;          // estação dona da boia
    uint8_t  sensorIdx = 0;           // 0..3
    bool     ligaQuandoAtivo = true;  // liga quando o sensor está ATIVO (= nível baixo)
    uint8_t  targetZoneId = 0;        // bomba/motor (zona de grupo OU GPO avulso)
    uint16_t minOnS = 30;             // tempo mínimo ligado (anti-chatter)
    uint16_t minOffS = 30;            // tempo mínimo desligado
    uint16_t staleTimeoutS = 90;      // sem leitura da boia → desliga + alerta
    char     mensagem[24] = {0};
};

class LevelControlTable {
  public:
    static constexpr size_t MAX = 4;
    static constexpr uint32_t MAGIC = 0x494C564C; // "ILVL"
    bool upsert(const LevelRule &r);   // por id (1..255); false = id 0 ou cheia
    bool removeById(uint8_t id);
    const LevelRule *byId(uint8_t id) const;
    const LevelRule *ruleAt(size_t index) const; // index-ésima regra ocupada
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // falha => tabela vazia

  private:
    LevelRule rules[MAX];
};
