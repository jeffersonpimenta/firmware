#pragma once
#include <stddef.h>
#include <stdint.h>
#include "modules/irrigation/IrrigationSettings.h"  // IrrigationSettings::LocalInterlock
#include "modules/irrigation/IrrigationProtocol.h"  // IrrigationProto::SensorReading

enum InterlockCond : uint8_t { COND_ATIVO = 0, COND_INATIVO = 1, COND_MENOR_QUE = 2, COND_MAIOR_QUE = 3 };
enum InterlockAcao : uint8_t { ACAO_BLOQUEAR_ABERTURA = 0, ACAO_FECHAR_E_BLOQUEAR = 1 };
enum InterlockTipo : uint8_t { IL_SENSOR = 0, IL_SIMULTANEIDADE = 1 };

// Avalia a condição de uma regra sobre uma leitura, com histerese latched.
// `active`/`valueCenti` vêm do sensor (digital usa active; analógico usa valueCenti).
// `latched` é o estado retido pelo chamador (entra e sai por referência).
// Retorna o novo estado de disparo (true = condição satisfeita agora).
// Nota: `histCenti` é ignorado para COND_ATIVO/COND_INATIVO (condições digitais) —
// histerese só se aplica às condições analógicas COND_MENOR_QUE/COND_MAIOR_QUE.
bool evalCondition(uint8_t condicao, bool active, int32_t valueCenti,
                   int32_t thresholdCenti, uint16_t histCenti, bool &latched);

constexpr size_t INTERLOCK_MAX = 16; // = InterlockTable::MAX (travado por static_assert no .cpp)

// Forward decls: InterlockTable.h inclui este header (enums), então NÃO incluímos
// InterlockTable.h aqui — evita include circular. Definição completa só no .cpp.
class InterlockTable;
struct InterlockRule;

// Snapshot de uma leitura de sensor de uma estação.
struct SensorSnapshot {
    uint32_t node = 0;
    uint8_t sensorIdx = 0;
    bool present = false;   // false = estação não reportou esse sensor
    bool active = false;    // digital
    int32_t valueCenti = 0; // analógico
};

struct ZoneVerdict {
    bool bloqueada = false;   // novo ciclo proibido (qualquer ação ativa)
    bool deveFechar = false;  // fechar_e_bloquear ativo
    uint8_t ruleId = 0;       // regra que disparou (0 = nenhuma)
};

class InterlockEngine {
  public:
    // `snaps`/`nSnaps`: leituras correntes. Atualiza latch interno e devolve
    // o cap efetivo de simultaneidade (0 = sem limite). Chamar 1×/tick.
    uint8_t evaluate(const InterlockTable &tbl, const SensorSnapshot *snaps, size_t nSnaps);
    // Veredito p/ uma zona, após evaluate().
    ZoneVerdict zoneVerdict(uint8_t zoneId) const;

  private:
    bool latched[INTERLOCK_MAX] = {false};
    bool fired[INTERLOCK_MAX] = {false};       // resultado do último evaluate por índice de regra ocupada
    const InterlockTable *lastTbl = nullptr;
    static const SensorSnapshot *findSnap(const SensorSnapshot *s, size_t n, uint32_t node, uint8_t idx);
    static bool ruleCoversZone(const InterlockRule &r, uint8_t zoneId);
};

struct LocalReplicaOut {
    uint8_t fecharMask = 0;   // saídas a fechar+bloquear agora
    uint8_t bloquearMask = 0; // saídas com abertura bloqueada
};

// Avalia as regras locais. `readings`/`nReadings`: leituras do SensorSampler local
// (id = sensorIdx). `latched`: array de estado retido pelo chamador (>= nRules). Puro.
LocalReplicaOut evalLocalInterlocks(const IrrigationSettings::LocalInterlock *rules, size_t nRules,
                                    const IrrigationProto::SensorReading *readings, size_t nReadings,
                                    bool *latched);
