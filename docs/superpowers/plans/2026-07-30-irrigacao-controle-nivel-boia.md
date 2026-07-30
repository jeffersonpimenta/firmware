# Controle de nível por boia (enchimento) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boia (sensor digital) num nó controla partida/parada automática de uma bomba (zona) em outro nó ou no GPO do gateway — enchimento de reservatório com histerese por tempo e fail-safe na perda de sinal.

**Architecture:** Core puro em TUs novos native-tested (`LevelControlTable` = modelo+persistência serializável; `LevelControlEngine` = máquina de estado start/stop que devolve *intents*). O `IrrigationModule` (gateway) resolve as leituras do `StationTelemetryCache`, chama o engine 1×/tick e executa os intents via o caminho de zona existente (`routeZoneToGroup` → grupo, ou `gwSendValveCmd` → zona livre). Config via codec web puro + endpoints CI-only + aba de painel.

**Tech Stack:** C++17, PlatformIO, Unity (suites nativas em `test/`), Meshtastic FSCom/LittleFS, JSON hand-rolled (`IrrigationWeb::JsonWriter`/`JsonReader`). Sem dependência nova.

**Design doc:** `docs/superpowers/specs/2026-07-30-irrigacao-controle-nivel-boia-design.md`

## Global Constraints

Copie no checklist mental de cada task:

- **Aditivo total.** Sem mudar a ABI de estação (settings v5 / 176 B). Sem novo tipo de wire. Protocolo VERSION continua 1. A tabela de nível persiste em arquivo próprio no LittleFS (como interlocks/grupos), não em settings.
- **Sem dependência nova.** JSON é hand-rolled: `IrrigationWeb::JsonWriter`/`JsonReader` (declarados em `modules/irrigation/IrrigationWebApi.h`).
- **Enums de auditoria/alerta são ABI append-only.** `AuditOrigin::NIVEL` e `AlertType::NIVEL_BOIA_MUDA` entram **no fim** do enum, nunca reordenar/remover (contrato com rótulos JS).
- **Pure-core / thin-glue.** `LevelControlTable`/`LevelControlEngine` não tocam hardware/rádio/FSCom → compilam e passam no build nativo. Tudo que toca `Channels`/rádio/FSCom/HTTP vive no `IrrigationModule`/endpoints (CI-only).
- **Fail-safe.** Teto de 120 min por abertura permanece; o RENEW só mantém vivo enquanto a boia diz "encher" **e** está fresca. Intertravamento de segurança ainda veta (via `routeZoneToGroup`).
- **Veredito da suíte nativa:** `./bin/run-tests.sh` → GREEN exit 0 (completa); `-f <suite>` → FILTERED exit 3 (única). No Windows rodar via Docker (memória `windows-native-test-docker`: `MSYS_NO_PATHCONV=1` + Docker Desktop; se o `test-native-docker.sh` quebrar no `cp -a`, usar `docker run` próprio com `tar --exclude=./.git --exclude=./.pio --exclude=./.claude --exclude=./.codegraph --exclude=./myfork`). Ao adicionar suite, **incrementar `test/native-suite-count`**.
- **Format** com `trunk fmt` antes de cada commit (não roda no host Windows — segue o clang-format do repo; CI do fork valida). Mensagens: `feat(irrigation): nível …`.

## File Structure

**Novos (sempre compilados — puros, native-tested):**
- `src/modules/irrigation/LevelControlTable.h` / `.cpp` — `struct LevelRule` + tabela (upsert/byId/ruleAt/count/serialize/deserialize). Espelha `InterlockTable`.
- `src/modules/irrigation/LevelControlEngine.h` / `.cpp` — máquina de estado pura; devolve `LevelIntent`.
- `test/test_level_control_table/test_main.cpp` — suite da tabela.
- `test/test_level_control/test_main.cpp` — suite do engine.

**Modificados:**
- `src/modules/irrigation/AuditLog.h` — `AuditOrigin::NIVEL` (append).
- `src/modules/irrigation/StationMonitor.h` — `AlertType::NIVEL_BOIA_MUDA` (append).
- `src/modules/irrigation/IrrigationGateway.h` — membros `levels` + `levelEngine`.
- `src/modules/irrigation/IrrigationWebApi.h` / `.cpp` — `buildLevelControls`/`parseLevelUpsert`/`parseLevelDelete`.
- `src/modules/irrigation/ServiceBackup.h` / `.cpp` — campo `niveisJson` no §5.5 backup.
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — persistência, apply-helpers, executor e avaliação no `gwTick`, backup.
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — endpoints CI-only `/api/irrigation/levels`.
- `data/irrigacao/` (`app.js` + `index.html`) — aba "Nível" + rótulos dos novos enums.
- `test/native-suite-count` — 62 → 64.

---

### Task 1: `LevelControlTable` (modelo + serialização pura) + suite

**Files:**
- Create: `src/modules/irrigation/LevelControlTable.h`, `src/modules/irrigation/LevelControlTable.cpp`
- Test: `test/test_level_control_table/test_main.cpp`
- Modify: `test/native-suite-count` (62 → 63)

**Interfaces:**
- Produces:
  `struct LevelRule { uint8_t id; uint32_t sensorNode; uint8_t sensorIdx; bool ligaQuandoAtivo; uint8_t targetZoneId; uint16_t minOnS; uint16_t minOffS; uint16_t staleTimeoutS; char mensagem[24]; }`
  `class LevelControlTable` com `static constexpr size_t MAX = 4;` `static constexpr uint32_t MAGIC = 0x494C564C;`, e `upsert/removeById/byId/ruleAt/count/serialize/deserialize`.

- [ ] **Step 1: Write the failing test**

`test/test_level_control_table/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/LevelControlTable.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static LevelRule mkRule(uint8_t id, uint32_t node, uint8_t target)
{
    LevelRule r{};
    r.id = id;
    r.sensorNode = node;
    r.sensorIdx = 1;
    r.ligaQuandoAtivo = true;
    r.targetZoneId = target;
    r.minOnS = 30;
    r.minOffS = 30;
    r.staleTimeoutS = 90;
    strncpy(r.mensagem, "cisterna", sizeof(r.mensagem) - 1);
    return r;
}

static void test_upsert_and_byId()
{
    LevelControlTable t;
    TEST_ASSERT_TRUE(t.upsert(mkRule(1, 0xaabbccdd, 5)));
    TEST_ASSERT_EQUAL_size_t(1, t.count());
    const LevelRule *r = t.byId(1);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX32(0xaabbccdd, r->sensorNode);
    TEST_ASSERT_EQUAL_UINT8(5, r->targetZoneId);
    TEST_ASSERT_NULL(t.byId(2));
    TEST_ASSERT_FALSE(t.upsert(mkRule(0, 1, 1))); // id 0 recusado
}

static void test_upsert_updates_in_place()
{
    LevelControlTable t;
    t.upsert(mkRule(1, 1, 5));
    LevelRule r = mkRule(1, 1, 9); // mesmo id, target novo
    TEST_ASSERT_TRUE(t.upsert(r));
    TEST_ASSERT_EQUAL_size_t(1, t.count());
    TEST_ASSERT_EQUAL_UINT8(9, t.byId(1)->targetZoneId);
}

static void test_full_table_rejects()
{
    LevelControlTable t;
    for (uint8_t i = 1; i <= LevelControlTable::MAX; i++)
        TEST_ASSERT_TRUE(t.upsert(mkRule(i, i, i)));
    TEST_ASSERT_FALSE(t.upsert(mkRule(99, 1, 1))); // cheia
}

static void test_remove()
{
    LevelControlTable t;
    t.upsert(mkRule(1, 1, 5));
    t.upsert(mkRule(2, 2, 6));
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_EQUAL_size_t(1, t.count());
    TEST_ASSERT_NULL(t.byId(1));
    TEST_ASSERT_FALSE(t.removeById(1)); // já removido
}

static void test_serialize_roundtrip()
{
    LevelControlTable t;
    t.upsert(mkRule(1, 0x11223344, 5));
    t.upsert(mkRule(3, 0x55667788, 7));
    uint8_t buf[256];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    LevelControlTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(2, t2.count());
    TEST_ASSERT_EQUAL_HEX32(0x55667788, t2.byId(3)->sensorNode);
}

static void test_deserialize_bad_crc_empties()
{
    LevelControlTable t;
    t.upsert(mkRule(1, 1, 5));
    uint8_t buf[256];
    size_t n = t.serialize(buf, sizeof(buf));
    buf[8] ^= 0xff; // corrompe um byte de dado
    LevelControlTable t2;
    TEST_ASSERT_FALSE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(0, t2.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_upsert_and_byId);
    RUN_TEST(test_upsert_updates_in_place);
    RUN_TEST(test_full_table_rejects);
    RUN_TEST(test_remove);
    RUN_TEST(test_serialize_roundtrip);
    RUN_TEST(test_deserialize_bad_crc_empties);
    exit(UNITY_END());
}
void loop() {}
```

`src/modules/irrigation/LevelControlTable.h` (inicial — só declarações para o teste linkar/falhar por falta do `.cpp`):
```cpp
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_level_control_table`
Expected: RED — link error, métodos de `LevelControlTable` indefinidos.

- [ ] **Step 3: Write minimal implementation**

`src/modules/irrigation/LevelControlTable.cpp` (espelha `InterlockTable.cpp`):
```cpp
#include "modules/irrigation/LevelControlTable.h"
#include "modules/irrigation/IrrigationProtocol.h" // crc32
#include <string.h>

size_t LevelControlTable::count() const
{
    size_t c = 0;
    for (auto &r : rules)
        if (r.id)
            c++;
    return c;
}

const LevelRule *LevelControlTable::byId(uint8_t id) const
{
    if (!id)
        return nullptr;
    for (auto &r : rules)
        if (r.id == id)
            return &r;
    return nullptr;
}

const LevelRule *LevelControlTable::ruleAt(size_t index) const
{
    for (auto &r : rules)
        if (r.id) {
            if (index == 0)
                return &r;
            index--;
        }
    return nullptr;
}

bool LevelControlTable::upsert(const LevelRule &in)
{
    if (!in.id)
        return false;
    for (auto &r : rules)
        if (r.id == in.id) {
            r = in;
            return true;
        }
    for (auto &r : rules)
        if (!r.id) {
            r = in;
            return true;
        }
    return false;
}

bool LevelControlTable::removeById(uint8_t id)
{
    if (!id)
        return false;
    for (auto &r : rules)
        if (r.id == id) {
            r = LevelRule{};
            return true;
        }
    return false;
}

// magic(4) + count(2) + N×sizeof(LevelRule) + crc32(4).
size_t LevelControlTable::serialize(uint8_t *buf, size_t cap) const
{
    size_t n = count();
    size_t need = 4 + 2 + n * sizeof(LevelRule) + 4;
    if (cap < need)
        return 0;
    size_t o = 0;
    uint32_t magic = MAGIC;
    memcpy(buf + o, &magic, 4);
    o += 4;
    uint16_t c = (uint16_t)n;
    memcpy(buf + o, &c, 2);
    o += 2;
    for (auto &r : rules)
        if (r.id) {
            memcpy(buf + o, &r, sizeof(r));
            o += sizeof(r);
        }
    uint32_t crc = IrrigationProto::crc32(buf, o);
    memcpy(buf + o, &crc, 4);
    o += 4;
    return o;
}

bool LevelControlTable::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &r : rules)
        r = LevelRule{};
    if (n < 10)
        return false;
    uint32_t magic;
    memcpy(&magic, buf, 4);
    if (magic != MAGIC)
        return false;
    uint16_t c;
    memcpy(&c, buf + 4, 2);
    size_t need = 4 + 2 + (size_t)c * sizeof(LevelRule) + 4;
    if (n != need || c > MAX)
        return false;
    uint32_t crc;
    memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4))
        return false;
    size_t o = 6;
    for (uint16_t i = 0; i < c; i++) {
        memcpy(&rules[i], buf + o, sizeof(LevelRule));
        o += sizeof(LevelRule);
    }
    return true;
}
```

Editar `test/native-suite-count`: trocar `62` por `63`.

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_level_control_table`
Expected: FILTERED (exit 3), 6 casos passam.

- [ ] **Step 5: Commit**

```bash
trunk fmt src/modules/irrigation/LevelControlTable.* test/test_level_control_table/test_main.cpp
git add src/modules/irrigation/LevelControlTable.* test/test_level_control_table/ test/native-suite-count
git commit -m "feat(irrigation): nível — LevelControlTable (modelo + serialização)"
```

---

### Task 2: `LevelControlEngine` (máquina de estado pura) + suite

Devolve *intents* start/stop/renew/stale a partir da tabela e de um array paralelo de leituras já resolvidas (`LevelInput`) — o glue faz o lookup no telemetry cache e o cálculo de frescor.

**Files:**
- Create: `src/modules/irrigation/LevelControlEngine.h`, `src/modules/irrigation/LevelControlEngine.cpp`
- Test: `test/test_level_control/test_main.cpp`
- Modify: `test/native-suite-count` (63 → 64)

**Interfaces:**
- Consumes: `LevelControlTable`, `LevelRule` (Task 1).
- Produces:
```cpp
struct LevelInput {           // leitura já resolvida da boia de UMA regra (índice = ruleAt(i))
    bool present = false;     // existe snapshot p/ (sensorNode, sensorIdx)
    bool active = false;      // estado digital corrente
    bool fresh = false;       // atMs do heartbeat dentro de staleTimeoutS
};
struct LevelIntent {
    uint8_t ruleId = 0;
    uint8_t zoneId = 0;
    enum class Act : uint8_t { NONE, START, RENEW, STOP, STALE_STOP } act = Act::NONE;
    uint16_t durS = 0;        // START/RENEW
};
class LevelControlEngine {
  public:
    static constexpr uint32_t RENEW_INTERVAL_MS = 60000;
    static constexpr uint16_t OPEN_CEILING_S = 7200; // == HydraulicGroupEngine::PUMP_CEILING_S
    // inputs[i] corresponde a tbl.ruleAt(i). Escreve até maxOut intents (1 por regra). Devolve a contagem.
    size_t evaluate(const LevelControlTable &tbl, const LevelInput *inputs, size_t nInputs, uint32_t nowMs,
                    LevelIntent *out, size_t maxOut);
  private:
    struct Rt {
        uint8_t id = 0;
        bool on = false;
        bool staleAlerted = false;
        uint32_t lastOnMs = 0, lastOffMs = 0, lastRenewMs = 0;
    };
    Rt rt[LevelControlTable::MAX];
    Rt &rtFor(uint8_t id);
};
```

- [ ] **Step 1: Write the failing test**

`test/test_level_control/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/LevelControlEngine.h"
#include "modules/irrigation/LevelControlTable.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

using Act = LevelIntent::Act;

static LevelRule rule1()
{
    LevelRule r{};
    r.id = 1;
    r.sensorNode = 0xaa;
    r.sensorIdx = 0;
    r.ligaQuandoAtivo = true; // ATIVO = nível baixo = liga
    r.targetZoneId = 5;
    r.minOnS = 30;
    r.minOffS = 30;
    r.staleTimeoutS = 90;
    return r;
}

// Helper: 1 regra, 1 input, devolve o único intent (ou NONE).
static LevelIntent step(LevelControlEngine &e, LevelControlTable &t, LevelInput in, uint32_t nowMs)
{
    LevelIntent out[LevelControlTable::MAX];
    size_t n = e.evaluate(t, &in, 1, nowMs, out, LevelControlTable::MAX);
    if (n == 0) {
        LevelIntent none{};
        return none;
    }
    return out[0];
}

static LevelInput freshInput(bool active) { return LevelInput{true, active, true}; }

static void test_starts_when_low_after_minOff()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    // t=0: nível baixo (ativo), minOff=30s não cumprido desde lastOff(0)? lastOff=0, now=1000ms<30000 → não liga
    TEST_ASSERT_EQUAL(Act::NONE, step(e, t, freshInput(true), 1000).act);
    // t=31s: minOff cumprido → START
    LevelIntent i = step(e, t, freshInput(true), 31000);
    TEST_ASSERT_EQUAL(Act::START, i.act);
    TEST_ASSERT_EQUAL_UINT8(5, i.zoneId);
    TEST_ASSERT_EQUAL_UINT16(LevelControlEngine::OPEN_CEILING_S, i.durS);
}

static void test_stops_when_full_after_minOn()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    step(e, t, freshInput(true), 31000);              // START @31s
    // nível encheu (inativo) @40s: minOn=30s desde lastOn(31s) não cumprido → segura (RENEW não, <60s)
    TEST_ASSERT_EQUAL(Act::NONE, step(e, t, freshInput(false), 40000).act);
    // @62s: minOn cumprido → STOP
    TEST_ASSERT_EQUAL(Act::STOP, step(e, t, freshInput(false), 62000).act);
}

static void test_renew_while_on()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    step(e, t, freshInput(true), 31000);              // START @31s, lastRenew=31s
    // @95s (>=31s+60s), ainda quer encher → RENEW
    LevelIntent i = step(e, t, freshInput(true), 95000);
    TEST_ASSERT_EQUAL(Act::RENEW, i.act);
    TEST_ASSERT_EQUAL_UINT16(LevelControlEngine::OPEN_CEILING_S, i.durS);
}

static void test_stale_stops_and_alerts_once()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    step(e, t, freshInput(true), 31000);              // START @31s (ligada)
    // boia sumiu: fresh=false → STALE_STOP (fecha + alerta)
    LevelInput stale{true, true, false};
    TEST_ASSERT_EQUAL(Act::STALE_STOP, step(e, t, stale, 40000).act);
    // ainda stale, já alertado e já desligado → NONE
    TEST_ASSERT_EQUAL(Act::NONE, step(e, t, stale, 45000).act);
    // volta fresco e baixo, minOff cumprido desde a parada(40s) → START de novo
    TEST_ASSERT_EQUAL(Act::START, step(e, t, freshInput(true), 80000).act);
}

static void test_absent_snapshot_treated_as_stale()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    step(e, t, freshInput(true), 31000);              // START
    LevelInput absent{false, false, false};
    TEST_ASSERT_EQUAL(Act::STALE_STOP, step(e, t, absent, 35000).act);
}

static void test_polarity_inverted()
{
    LevelControlEngine e;
    LevelControlTable t;
    LevelRule r = rule1();
    r.ligaQuandoAtivo = false; // liga quando INATIVO
    t.upsert(r);
    // ativo agora significa "não ligar"
    TEST_ASSERT_EQUAL(Act::NONE, step(e, t, freshInput(true), 31000).act);
    // inativo → quer ligar; minOff ok → START
    TEST_ASSERT_EQUAL(Act::START, step(e, t, freshInput(false), 62000).act);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_starts_when_low_after_minOff);
    RUN_TEST(test_stops_when_full_after_minOn);
    RUN_TEST(test_renew_while_on);
    RUN_TEST(test_stale_stops_and_alerts_once);
    RUN_TEST(test_absent_snapshot_treated_as_stale);
    RUN_TEST(test_polarity_inverted);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_level_control`
Expected: RED — `LevelControlEngine` indefinido.

- [ ] **Step 3: Write minimal implementation**

`src/modules/irrigation/LevelControlEngine.h` — as definições do bloco **Produces** acima (com `#pragma once`, `#include "modules/irrigation/LevelControlTable.h"`, `#include <stdint.h>`, `#include <stddef.h>`).

`src/modules/irrigation/LevelControlEngine.cpp`:
```cpp
#include "modules/irrigation/LevelControlEngine.h"

LevelControlEngine::Rt &LevelControlEngine::rtFor(uint8_t id)
{
    for (auto &s : rt)
        if (s.id == id)
            return s;
    for (auto &s : rt)
        if (s.id == 0) {
            s = Rt{};
            s.id = id;
            return s;
        }
    return rt[0]; // não deve acontecer (rt dimensionado = tabela)
}

size_t LevelControlEngine::evaluate(const LevelControlTable &tbl, const LevelInput *inputs, size_t nInputs,
                                    uint32_t nowMs, LevelIntent *out, size_t maxOut)
{
    size_t no = 0;
    for (size_t i = 0; i < tbl.count() && i < nInputs; i++) {
        const LevelRule *r = tbl.ruleAt(i);
        if (!r)
            break;
        Rt &s = rtFor(r->id);
        const LevelInput &in = inputs[i];

        // --- Boia muda/ausente: fail-safe desliga + alerta 1×/episódio ---
        if (!in.present || !in.fresh) {
            if (s.on || !s.staleAlerted) {
                if (no < maxOut)
                    out[no++] = LevelIntent{r->id, r->targetZoneId, LevelIntent::Act::STALE_STOP, 0};
                s.on = false;
                s.lastOffMs = nowMs;
                s.staleAlerted = true;
            }
            continue;
        }
        s.staleAlerted = false;

        bool wantOn = (in.active == r->ligaQuandoAtivo);

        if (!s.on) {
            if (wantOn && (uint32_t)(nowMs - s.lastOffMs) >= (uint32_t)r->minOffS * 1000u) {
                if (no < maxOut)
                    out[no++] = LevelIntent{r->id, r->targetZoneId, LevelIntent::Act::START, OPEN_CEILING_S};
                s.on = true;
                s.lastOnMs = nowMs;
                s.lastRenewMs = nowMs;
            }
        } else {
            if (!wantOn && (uint32_t)(nowMs - s.lastOnMs) >= (uint32_t)r->minOnS * 1000u) {
                if (no < maxOut)
                    out[no++] = LevelIntent{r->id, r->targetZoneId, LevelIntent::Act::STOP, 0};
                s.on = false;
                s.lastOffMs = nowMs;
            } else if (wantOn && (uint32_t)(nowMs - s.lastRenewMs) >= RENEW_INTERVAL_MS) {
                if (no < maxOut)
                    out[no++] = LevelIntent{r->id, r->targetZoneId, LevelIntent::Act::RENEW, OPEN_CEILING_S};
                s.lastRenewMs = nowMs;
            }
        }
    }
    return no;
}
```

Editar `test/native-suite-count`: trocar `63` por `64`.

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_level_control`
Expected: FILTERED, 6 casos passam.

- [ ] **Step 5: Commit**

```bash
trunk fmt src/modules/irrigation/LevelControlEngine.* test/test_level_control/test_main.cpp
git add src/modules/irrigation/LevelControlEngine.* test/test_level_control/ test/native-suite-count
git commit -m "feat(irrigation): nível — LevelControlEngine (start/stop/renew/stale)"
```

---

### Task 3: codec web `buildLevelControls` / `parseLevelUpsert` / `parseLevelDelete` (puro) + casos

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`, `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `IrrigationWeb::JsonWriter`, `IrrigationWeb::JsonReader`, `IrrigationWeb::ParseResult`, `LevelControlTable`/`LevelRule`.
- Produces (namespace `IrrigationWeb`):
  `size_t buildLevelControls(const LevelControlTable &tbl, char *buf, size_t cap);`
  `ParseResult parseLevelUpsert(const char *json, size_t len, LevelRule &out);`
  `ParseResult parseLevelDelete(const char *json, size_t len, uint8_t &outId);`

- [ ] **Step 1: Write the failing test** (append em `test/test_irrigation_webapi/test_main.cpp`, registrar em `setup()`)

```cpp
// no topo do arquivo, junto dos outros includes:
#include "modules/irrigation/LevelControlTable.h"

static void test_buildLevelControls_shape()
{
    LevelControlTable t;
    LevelRule r{};
    r.id = 1;
    r.sensorNode = 0xa1b2c3d4;
    r.sensorIdx = 2;
    r.ligaQuandoAtivo = true;
    r.targetZoneId = 5;
    r.minOnS = 45;
    r.minOffS = 60;
    r.staleTimeoutS = 120;
    strncpy(r.mensagem, "cisterna baixa", sizeof(r.mensagem) - 1);
    t.upsert(r);
    char buf[512];
    size_t n = IrrigationWeb::buildLevelControls(t, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = 0;
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"targetZoneId\":5"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sensorIdx\":2"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ligaQuandoAtivo\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"minOffS\":60"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "cisterna baixa"));
}

static void test_parseLevelUpsert_ok()
{
    const char *j = "{\"id\":0,\"sensorNode\":2712847316,\"sensorIdx\":2,\"ligaQuandoAtivo\":true,"
                    "\"targetZoneId\":5,\"minOnS\":45,\"minOffS\":60,\"staleTimeoutS\":120,"
                    "\"mensagem\":\"poco\"}";
    LevelRule out{};
    IrrigationWeb::ParseResult pr = IrrigationWeb::parseLevelUpsert(j, strlen(j), out);
    TEST_ASSERT_TRUE(pr.ok);
    TEST_ASSERT_EQUAL_UINT8(0, out.id); // id 0 = servidor aloca
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, out.sensorNode);
    TEST_ASSERT_EQUAL_UINT8(5, out.targetZoneId);
    TEST_ASSERT_TRUE(out.ligaQuandoAtivo);
    TEST_ASSERT_EQUAL_UINT16(60, out.minOffS);
    TEST_ASSERT_EQUAL_STRING("poco", out.mensagem);
}

static void test_parseLevelUpsert_rejects_no_target()
{
    const char *j = "{\"id\":1,\"sensorNode\":1,\"sensorIdx\":0,\"targetZoneId\":0}";
    LevelRule out{};
    IrrigationWeb::ParseResult pr = IrrigationWeb::parseLevelUpsert(j, strlen(j), out);
    TEST_ASSERT_FALSE(pr.ok);
}

static void test_parseLevelDelete_ok()
{
    const char *j = "{\"id\":3}";
    uint8_t id = 0;
    IrrigationWeb::ParseResult pr = IrrigationWeb::parseLevelDelete(j, strlen(j), id);
    TEST_ASSERT_TRUE(pr.ok);
    TEST_ASSERT_EQUAL_UINT8(3, id);
}
```

Registrar no `setup()`:
```cpp
    RUN_TEST(test_buildLevelControls_shape);
    RUN_TEST(test_parseLevelUpsert_ok);
    RUN_TEST(test_parseLevelUpsert_rejects_no_target);
    RUN_TEST(test_parseLevelDelete_ok);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED — funções indefinidas.

- [ ] **Step 3: Write minimal implementation**

Em `IrrigationWebApi.h`, adicionar o include `#include "modules/irrigation/LevelControlTable.h"` (junto dos outros includes de tabela) e, dentro de `namespace IrrigationWeb`, as três declarações do bloco **Produces**.

Em `IrrigationWebApi.cpp` (perto de `buildGroups`/`parseGroupUpsert`):
```cpp
size_t buildLevelControls(const LevelControlTable &tbl, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < tbl.count(); i++) {
        const LevelRule *r = tbl.ruleAt(i);
        if (!r)
            break;
        w.beginObject();
        w.keyNum("id", r->id);
        w.keyNum("sensorNode", r->sensorNode);
        w.keyNum("sensorIdx", r->sensorIdx);
        w.keyBool("ligaQuandoAtivo", r->ligaQuandoAtivo);
        w.keyNum("targetZoneId", r->targetZoneId);
        w.keyNum("minOnS", r->minOnS);
        w.keyNum("minOffS", r->minOffS);
        w.keyNum("staleTimeoutS", r->staleTimeoutS);
        w.keyStr("mensagem", r->mensagem);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

ParseResult parseLevelUpsert(const char *json, size_t len, LevelRule &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = -1, node = 0, sidx = 0, target = 0;
    int64_t minOn = 30, minOff = 30, stale = 90;
    int64_t liga = 1;
    char msg[24] = {0};

    if (!rd.getInt("id", id) || id < 0 || id > (int64_t)LevelControlTable::MAX)
        r.fail("id invalido (0..4)");
    rd.getInt("sensorNode", node);
    rd.getInt("sensorIdx", sidx);
    rd.getInt("ligaQuandoAtivo", liga); // aceita 0/1 (JsonReader trata true/false como 1/0)
    rd.getInt("targetZoneId", target);
    rd.getInt("minOnS", minOn);
    rd.getInt("minOffS", minOff);
    rd.getInt("staleTimeoutS", stale);
    rd.getStr("mensagem", msg, sizeof(msg));

    if (sidx < 0 || sidx > 3)
        r.fail("sensorIdx invalido (0..3)");
    if (node == 0)
        r.fail("sensorNode ausente");
    if (target < 1 || target > 255)
        r.fail("targetZoneId ausente");
    if (!r.ok)
        return r;

    out = LevelRule{};
    out.id = (uint8_t)id;
    out.sensorNode = (uint32_t)node;
    out.sensorIdx = (uint8_t)sidx;
    out.ligaQuandoAtivo = (liga != 0);
    out.targetZoneId = (uint8_t)target;
    out.minOnS = (uint16_t)(minOn < 0 ? 0 : minOn);
    out.minOffS = (uint16_t)(minOff < 0 ? 0 : minOff);
    out.staleTimeoutS = (uint16_t)(stale <= 0 ? 90 : stale);
    snprintf(out.mensagem, sizeof(out.mensagem), "%s", msg);
    return r;
}

ParseResult parseLevelDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > (int64_t)LevelControlTable::MAX) {
        r.fail("id invalido (1..4)");
        return r;
    }
    outId = (uint8_t)id;
    return r;
}
```

Nota: se `JsonWriter` não tiver `keyBool`, emitir com `w.key("ligaQuandoAtivo"); w.raw(r->ligaQuandoAtivo ? "true" : "false");` (verificar a API existente em `IrrigationWebApi.h`; `buildNodeState`/`buildAlerts` já emitem booleanos — reusar o mesmo método).

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, todos passam (incl. os 4 novos).

- [ ] **Step 5: Commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.* test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.* test/test_irrigation_webapi/
git commit -m "feat(irrigation): nível — codec web build/parse de regras"
```

---

### Task 4: enums (auditoria/alerta) + agregado gateway + persistência

Peça de infra que Task 5 precisa: os enums novos, os membros no `IrrigationGateway`, e o par load/save no LittleFS. Sem lógica de tick ainda.

**Files:**
- Modify: `src/modules/irrigation/AuditLog.h`, `src/modules/irrigation/StationMonitor.h`,
  `src/modules/irrigation/IrrigationGateway.h`, `src/modules/irrigation/IrrigationModule.h`,
  `src/modules/irrigation/IrrigationModule.cpp`

**Interfaces:**
- Produces: `AuditOrigin::NIVEL`; `AlertType::NIVEL_BOIA_MUDA`; `IrrigationGateway::levels` (`LevelControlTable`) e `IrrigationGateway::levelEngine` (`LevelControlEngine`); `IrrigationModule::loadLevels()`/`saveLevels()`.

- [ ] **Step 1: Editar os enums (append-only)**

`src/modules/irrigation/AuditLog.h` — adicionar `NIVEL` ao fim de `AuditOrigin`:
```cpp
enum class AuditOrigin : uint8_t {
    SISTEMA = 0, CRONOGRAMA, PAINEL, PORTAL_CAMPO, BOTAO_FISICO,
    ENTRADA_FISICA, INTERTRAVAMENTO, FAILSAFE_TIMER, SERVICO,
    GRUPO_HIDRAULICO, // = 9
    NIVEL             // = 10 (controle de nível por boia) — append-only
};
```

`src/modules/irrigation/StationMonitor.h` — adicionar `NIVEL_BOIA_MUDA` ao fim de `AlertType`:
```cpp
enum class AlertType : uint8_t {
    NONE = 0, BATT_AVISO, BATT_CRITICO, BATT_HIBERNACAO, BATT_RECUPEROU,
    SILENT, BACK_ONLINE, REBOOT_ANOMALY, CMD_FAIL, CONFIG_ADOPTED,
    NIVEL_BOIA_MUDA // = 10 — boia muda além do timeout (fail-safe desligou a bomba)
};
```

- [ ] **Step 2: Agregado gateway**

`src/modules/irrigation/IrrigationGateway.h` — adicionar o include e os membros:
```cpp
#include "modules/irrigation/LevelControlEngine.h"
#include "modules/irrigation/LevelControlTable.h"
```
e, dentro de `struct IrrigationGateway`, após os membros de grupo:
```cpp
    // Controle de nível por boia (enchimento automático).
    LevelControlTable levels;
    LevelControlEngine levelEngine;
```

- [ ] **Step 3: Persistência (espelha loadGroups/saveGroups)**

Em `IrrigationModule.cpp`, junto das outras constantes de path (perto de `GW_GRUPOS_PATH`, ~linha 45):
```cpp
static const char *GW_NIVEIS_PATH = "/prefs/irrigation_niveis.dat";
static const char *GW_NIVEIS_TMP = "/prefs/irrigation_niveis.tmp";
```

Declarar em `IrrigationModule.h` (junto de `loadGroups`/`saveGroups`):
```cpp
    bool loadLevels();
    bool saveLevels();
```

Implementar em `IrrigationModule.cpp` (após `saveGroups`):
```cpp
// Persistência da tabela de controle de nível (arquivo separado). Espelha loadGroups/saveGroups.
bool IrrigationModule::loadLevels()
{
    size_t n = 0;
    uint8_t buf[6 + LevelControlTable::MAX * sizeof(LevelRule) + 4];
    if (!stagedRead(GW_NIVEIS_PATH, buf, sizeof(buf), n))
        return false; // ausente na 1ª init — ok, tabela vazia
    return gateway.levels.deserialize(buf, n);
}

bool IrrigationModule::saveLevels()
{
    uint8_t buf[6 + LevelControlTable::MAX * sizeof(LevelRule) + 4];
    size_t n = gateway.levels.serialize(buf, sizeof(buf));
    return stagedWrite(GW_NIVEIS_TMP, GW_NIVEIS_PATH, buf, n);
}
```

Chamar `loadLevels();` no init do gateway, imediatamente após `loadGroups();` (~linha 254).

- [ ] **Step 4: Compilar (build nativo linka o módulo)**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED e verde — este passo só garante que o header/enum/agregado novos **compilam** no build nativo (o `IrrigationModule` é linkado pela suíte). Sem casos novos aqui.

- [ ] **Step 5: Commit**

```bash
trunk fmt src/modules/irrigation/AuditLog.h src/modules/irrigation/StationMonitor.h src/modules/irrigation/IrrigationGateway.h src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git add src/modules/irrigation/AuditLog.h src/modules/irrigation/StationMonitor.h src/modules/irrigation/IrrigationGateway.h src/modules/irrigation/IrrigationModule.*
git commit -m "feat(irrigation): nível — enums, agregado gateway e persistência"
```

---

### Task 5: avaliação no `gwTick` + executor de zona + apply-helpers

O coração do glue: resolve leituras do telemetry cache, chama o engine, executa intents (grupo ou zona livre), fecha+alerta no stale. Não é native-testável (toca rádio/estado do gateway) — validado por compilação no nativo + banca.

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `src/modules/irrigation/IrrigationModule.cpp`

**Interfaces:**
- Consumes: `gateway.levels`, `gateway.levelEngine`, `gateway.telemetry` (`StationTelemetryCache::byNode` → `StationTelemetry{atMs, sensors[], sensorCount}`), `routeZoneToGroup`, `gwSendValveCmd`, `gateway.zones`, `gateway.stations`, `gateway.openGate`, `gateway.alerts.push`, `auditEvent`, `saveLevels`.
- Produces: `IrrigationModule::gwLevelTick(uint32_t nowMs)`; `IrrigationModule::gwDriveZone(uint8_t zoneId, bool open, uint16_t durS)`; `IrrigationModule::gwApplyLevelUpsert(LevelRule&, char* err, size_t)`; `IrrigationModule::gwApplyLevelDelete(uint8_t id)`.

- [ ] **Step 1: Declarar em `IrrigationModule.h`** (junto de `routeZoneToGroup` e dos `gwApplyGroup*`)

```cpp
    void gwLevelTick(uint32_t nowMs);
    void gwDriveZone(uint8_t zoneId, bool open, uint16_t durS);
    bool gwApplyLevelUpsert(LevelRule &r, char *err, size_t errCap); // aloca id se 0; salva
    bool gwApplyLevelDelete(uint8_t id);
```
(garantir `#include "modules/irrigation/LevelControlTable.h"` no `.h`, ou forward-decl `struct LevelRule;` se preferir manter o header leve — `LevelControlTable.h` já vem via `IrrigationGateway.h`.)

- [ ] **Step 2: Executor de zona** — em `IrrigationModule.cpp`, espelha o caminho OPEN/CLOSE do scheduler (linhas 2385-2407):

```cpp
// Abre/fecha uma zona pelo caminho canônico: grupo hidráulico se for zona-membro,
// senão comando direto de válvula/GPO. Espelha o bloco OPEN/CLOSE do scheduler.
void IrrigationModule::gwDriveZone(uint8_t zoneId, bool open, uint16_t durS)
{
    const Zone *z = gateway.zones.byId(zoneId);
    if (!z)
        return;
    const StationEntry *st = gateway.stations.byNode(z->node);
    uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
    if (open) {
        if (routeZoneToGroup(zoneId, true, durS)) // grupo cuida da coreografia da bomba
            return;
        gwSendValveCmd(z->node, z->index, z->tipo, 1, durS, z->id, attempts);
    } else {
        if (routeZoneToGroup(zoneId, false, 0))
            return;
        gateway.openGate.release(zoneId);
        gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
    }
}
```

- [ ] **Step 3: Tick de nível** — resolve leituras + chama o engine + executa intents:

```cpp
// Avalia as regras de controle de nível 1×/tick. Resolve cada boia no cache de
// telemetria (leitura digital + frescor via atMs) e executa os intents do engine.
void IrrigationModule::gwLevelTick(uint32_t nowMs)
{
    if (gateway.levels.count() == 0)
        return;

    LevelInput inputs[LevelControlTable::MAX];
    for (size_t i = 0; i < gateway.levels.count() && i < LevelControlTable::MAX; i++) {
        const LevelRule *r = gateway.levels.ruleAt(i);
        LevelInput in{};
        const StationTelemetry *t = gateway.telemetry.byNode(r->sensorNode);
        if (t && t->node) {
            for (uint8_t k = 0; k < t->sensorCount && k < IrrigationProto::HB_MAX_SENSORS; k++)
                if (t->sensors[k].id == r->sensorIdx) {
                    in.present = true;
                    in.active = (t->sensors[k].valueCenti != 0);
                    break;
                }
            in.fresh = ((uint32_t)(nowMs - t->atMs) <= (uint32_t)r->staleTimeoutS * 1000u);
        }
        inputs[i] = in;
    }

    LevelIntent out[LevelControlTable::MAX];
    size_t n = gateway.levelEngine.evaluate(gateway.levels, inputs, gateway.levels.count(), nowMs, out,
                                            LevelControlTable::MAX);
    for (size_t i = 0; i < n; i++) {
        const LevelIntent &it = out[i];
        switch (it.act) {
        case LevelIntent::Act::START:
        case LevelIntent::Act::RENEW:
            gwDriveZone(it.zoneId, true, it.durS);
            if (it.act == LevelIntent::Act::START) {
                const Zone *z = gateway.zones.byId(it.zoneId);
                auditEvent(AuditOrigin::NIVEL, AuditAction::ABRIR, it.zoneId, AuditResult::OK, z ? z->node : 0);
            }
            break;
        case LevelIntent::Act::STOP:
            gwDriveZone(it.zoneId, false, 0);
            {
                const Zone *z = gateway.zones.byId(it.zoneId);
                auditEvent(AuditOrigin::NIVEL, AuditAction::FECHAR, it.zoneId, AuditResult::OK, z ? z->node : 0);
            }
            break;
        case LevelIntent::Act::STALE_STOP: {
            gwDriveZone(it.zoneId, false, 0);
            const Zone *z = gateway.zones.byId(it.zoneId);
            auditEvent(AuditOrigin::NIVEL, AuditAction::FECHAR, it.zoneId, AuditResult::TIMEOUT, z ? z->node : 0);
            const LevelRule *r = gateway.levels.byId(it.ruleId);
            Alert a{};
            a.type = AlertType::NIVEL_BOIA_MUDA;
            a.node = r ? r->sensorNode : 0;
            a.arg = it.zoneId;
            a.atMs = nowMs;
            gateway.alerts.push(a);
            break;
        }
        case LevelIntent::Act::NONE:
            break;
        }
    }
}
```

- [ ] **Step 4: Chamar no `gwTick`** — logo **após** o bloco de intertravamentos (após a linha ~2677, antes do scheduler), para que o veto de segurança já esteja calculado:

```cpp
    // --- Controle de nível por boia (enchimento) — após intertravamentos ---
    gwLevelTick(millis());
```

- [ ] **Step 5: Apply-helpers** (usados pelos endpoints da Task 6) — espelham `gwApplyGroupUpsert`/`gwApplyGroupDelete`:

```cpp
bool IrrigationModule::gwApplyLevelUpsert(LevelRule &r, char *err, size_t errCap)
{
    if (r.id == 0) { // aloca menor id livre 1..MAX
        for (uint8_t cand = 1; cand <= LevelControlTable::MAX; cand++)
            if (!gateway.levels.byId(cand)) {
                r.id = cand;
                break;
            }
        if (r.id == 0) {
            snprintf(err, errCap, "tabela de niveis cheia");
            return false;
        }
    }
    if (!gateway.levels.upsert(r)) {
        snprintf(err, errCap, "tabela de niveis cheia");
        return false;
    }
    saveLevels();
    return true;
}

bool IrrigationModule::gwApplyLevelDelete(uint8_t id)
{
    if (!gateway.levels.removeById(id))
        return false;
    saveLevels();
    return true;
}
```

- [ ] **Step 6: Compilar no nativo**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED e verde — garante que o glue novo **compila+linka** no build nativo (globais `gateway`, `auditEvent`, `gwSendValveCmd` existem no nativo). Sem casos novos (lógica de rádio validada em banca).

- [ ] **Step 7: Commit**

```bash
trunk fmt src/modules/irrigation/IrrigationModule.*
git add src/modules/irrigation/IrrigationModule.*
git commit -m "feat(irrigation): nível — avaliação no gwTick, executor de zona e apply-helpers"
```

---

### Task 6: backup §5.5 — campo `niveisJson` (native-tested)

Inclui as regras de nível no backup completo do cliente (§5.5), aditivo. Perfis antigos sem o campo carregam com tabela vazia (o scanner só carrega opaco).

**Files:**
- Modify: `src/modules/irrigation/ServiceBackup.h`, `src/modules/irrigation/ServiceBackup.cpp`,
  `src/modules/irrigation/IrrigationModule.cpp` (`gwBuildBackup`)
- Test: `test/test_service_backup/test_main.cpp`

**Interfaces:**
- Modifies: `IrrigationService::BackupSource` ganha `const char *niveisJson;`; `buildClientBackup` emite `"niveis"` dentro de `"config"`.

- [ ] **Step 1: Write the failing test** (append em `test/test_service_backup/test_main.cpp`, registrar no `setup()`)

```cpp
static void test_buildClientBackup_carries_niveis()
{
    BackupSource s{};
    s.id = "f1";
    s.nome = "A";
    s.canalNome = "c1";
    s.pskB64 = "1PG7Og==";
    s.preset = 0;
    s.gateway = 0xa1b2c3d4;
    s.estacoesJson = "[]";
    s.snapshotEpochJson = "{}";
    s.zonasJson = "[]";
    s.programasJson = "[]";
    s.intertravamentosJson = "[]";
    s.gruposJson = "[]";
    s.sensorNamesJson = "[]";
    s.niveisJson = "[{\"id\":1,\"targetZoneId\":5}]";
    s.seqJson = "";
    char buf[1024];
    size_t n = buildClientBackup(s, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    IrrigationService::Slice cfg;
    TEST_ASSERT_TRUE(jsonMember(buf, n, "config", cfg));
    IrrigationService::Slice niveis;
    TEST_ASSERT_TRUE(jsonMember(cfg.p, cfg.n, "niveis", niveis));
    TEST_ASSERT_TRUE(niveis.n > 2); // não é "[]"
}
```

Registrar: `RUN_TEST(test_buildClientBackup_carries_niveis);`

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_service_backup`
Expected: RED — `BackupSource` não tem `niveisJson` (erro de compilação) ou `"niveis"` ausente.

- [ ] **Step 3: Write minimal implementation**

Em `ServiceBackup.h`, no `struct BackupSource`, adicionar após `sensorNamesJson`:
```cpp
    const char *niveisJson; // "[{...}]" ou "[]"
```

Em `ServiceBackup.cpp`, dentro de `buildClientBackup`, no objeto `config`, após o bloco `sensorNames`:
```cpp
    w.key("niveis");
    w.raw(s.niveisJson ? s.niveisJson : "[]");
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_service_backup`
Expected: FILTERED, todos passam.

- [ ] **Step 5: Wire `gwBuildBackup`** — em `IrrigationModule.cpp` (`gwBuildBackup`, ~linha 1529), montar o JSON das regras de nível e passar em `BackupSource`:

```cpp
    char niveisBuf[512];
    IrrigationWeb::buildLevelControls(gateway.levels, niveisBuf, sizeof(niveisBuf));
    // ... onde src é o BackupSource sendo preenchido:
    src.niveisJson = niveisBuf;
```
(inserir junto das outras atribuições `src.gruposJson = ...`; garantir que `niveisBuf` viva até a chamada de `buildClientBackup`.)

- [ ] **Step 6: Compilar no nativo** (o glue de `gwBuildBackup` só compila no build cheio, mas o link nativo do módulo valida a assinatura)

Run: `./bin/run-tests.sh -f test_service_backup`
Expected: FILTERED e verde.

- [ ] **Step 7: Commit**

```bash
trunk fmt src/modules/irrigation/ServiceBackup.* src/modules/irrigation/IrrigationModule.cpp test/test_service_backup/test_main.cpp
git add src/modules/irrigation/ServiceBackup.* src/modules/irrigation/IrrigationModule.cpp test/test_service_backup/
git commit -m "feat(irrigation): nível — regras no backup 5.5 do cliente"
```

---

### Task 7: endpoints CI-only + aba "Nível" no painel

Fiação HTTP + UI. **CI/banca-only** — excluída do build nativo, não native-testável. Validada por compilação de CI do fork + banca.

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp`
- Modify: `data/irrigacao/app.js`, `data/irrigacao/index.html`

**Interfaces:**
- Consumes: `buildLevelControls`, `parseLevelUpsert`, `parseLevelDelete`, `gwApplyLevelUpsert`, `gwApplyLevelDelete`, `irrigationModule->gwState().levels`.

- [ ] **Step 1: Handlers** — em `IrrigationWebEndpoints.cpp`, espelhando `hGroupsGet`/`hGroupsPost`/`hGroupsDelete` (linhas 574-651):

```cpp
// GET /api/irrigation/levels — lista de regras de controle de nível
static void hLevelsGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    char buf[1024];
    size_t n = buildLevelControls(irrigationModule->gwState().levels, buf, sizeof(buf));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// POST /api/irrigation/levels — upsert (id=0 => servidor aloca)
static void hLevelsPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[512];
    size_t nb = readBody(req, body, sizeof(body));
    LevelRule r;
    ParseResult pr = parseLevelUpsert(body, nb, r);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    char err[48] = {0};
    if (!irrigationModule->gwApplyLevelUpsert(r, err, sizeof(err))) {
        char out[128];
        JsonWriter w(out, sizeof(out));
        w.beginObject(); w.key("errors"); w.beginArray(); w.str(err[0] ? err : "erro"); w.endArray(); w.endObject();
        w.done();
        sendJson(res, out, 400);
        return;
    }
    char out[1024];
    size_t n = buildLevelControls(irrigationModule->gwState().levels, out, sizeof(out));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, out);
}

// POST /api/irrigation/levels/delete — remove por id
static void hLevelsDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0;
    ParseResult pr = parseLevelDelete(body, nb, id);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->gwApplyLevelDelete(id)) {
        sendJson(res, "{\"errors\":[\"regra inexistente\"]}", 400);
        return;
    }
    char out[1024];
    size_t n = buildLevelControls(irrigationModule->gwState().levels, out, sizeof(out));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, out);
}
```

Registrar junto dos `registerNode` de grupos (linhas 761-765):
```cpp
    server->registerNode(new ResourceNode("/api/irrigation/levels", "GET", &hLevelsGet));
    server->registerNode(new ResourceNode("/api/irrigation/levels", "POST", &hLevelsPost));
    server->registerNode(new ResourceNode("/api/irrigation/levels/delete", "POST", &hLevelsDelete));
```

- [ ] **Step 2: Aba "Nível" no painel** — em `data/irrigacao/index.html`, adicionar a entrada no menu "Mais" (junto de Grupos/Intertrav./Sensores) apontando para `showSub('niveis')`; e em `data/irrigacao/app.js`, uma função `renderNiveis()` que segue o padrão de `renderGrupos()` (GET `/api/irrigation/levels`, tabela com colunas boia (nó+idx), zona-alvo, polaridade, min-on/off, timeout, mensagem; form de upsert POST `/api/irrigation/levels`; botão excluir POST `/api/irrigation/levels/delete`). Reutilizar os helpers `apiGet`/`apiPost` e o padrão de render de `renderGrupos`.

Exemplo mínimo de `renderNiveis` (adaptar aos helpers reais do arquivo):
```javascript
async function renderNiveis() {
  const rules = await apiGet('/api/irrigation/levels');
  const rows = rules.map(r => `
    <tr>
      <td>!${r.sensorNode.toString(16).padStart(8,'0')} / s${r.sensorIdx}</td>
      <td>zona ${r.targetZoneId}</td>
      <td>${r.ligaQuandoAtivo ? 'ativo=baixo' : 'inativo=baixo'}</td>
      <td>${r.minOnS}s / ${r.minOffS}s</td>
      <td>${r.staleTimeoutS}s</td>
      <td>${r.mensagem||''}</td>
      <td><button onclick="delNivel(${r.id})">excluir</button></td>
    </tr>`).join('');
  el('#view').innerHTML = `<h2>Controle de nível</h2>
    <table><thead><tr><th>Boia</th><th>Bomba</th><th>Liga</th><th>Min on/off</th>
    <th>Timeout</th><th>Msg</th><th></th></tr></thead><tbody>${rows}</tbody></table>
    ${nivelForm()}`;
}
async function delNivel(id){ await apiPost('/api/irrigation/levels/delete',{id}); renderNiveis(); }
```

- [ ] **Step 3: Rótulos dos novos enums** — em `data/irrigacao/app.js`, adicionar às tabelas de rótulos existentes:
  - rótulo de `AuditOrigin` índice 10 → `"nível"` (na tabela usada por `renderLog`).
  - rótulo de `AlertType` `NIVEL_BOIA_MUDA` → texto tipo `"boia sem sinal — bomba desligada"` (na tabela usada por `buildAlerts`/overview).

- [ ] **Step 4: Commit** (sem teste nativo — CI/banca-only)

```bash
git add src/modules/irrigation/IrrigationWebEndpoints.cpp data/irrigacao/app.js data/irrigacao/index.html
git commit -m "feat(irrigation): nível — endpoints CI-only e aba do painel"
```

---

### Task 8: verificação final — suíte nativa completa + roadmap

**Files:**
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (nota da feature)

- [ ] **Step 1: Rodar a suíte nativa completa** (Docker no Windows)

Run: `./bin/run-tests.sh`
Expected: GREEN, exit 0. `test/native-suite-count` = 64; as duas suites novas (`test_level_control_table`, `test_level_control`) contadas; `test_irrigation_webapi` e `test_service_backup` com os casos novos.

- [ ] **Step 2: Nota no roadmap** — registrar a feature (controle de nível por boia, aditivo pós-Fase 8), com a pendência de banca.

- [ ] **Step 3: Commit + push ao fork**

```bash
git add docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "docs(irrigation): nível — nota de roadmap + verificação da suíte"
git push fork sistema-irrigacao
```

- [ ] **Step 4: Checklist de banca 2 nós** (não native-testável — registrar como pendência)
  - Boia baixa (ativo) → bomba liga (após `minOffS`).
  - Boia alta (inativo) → bomba desliga (após `minOnS`), sem chatter no ponto de corte.
  - Nó da boia desligado > `staleTimeoutS` → bomba desliga + alerta "boia sem sinal".
  - Boia travada em "baixo" → teto de 120 min corta a bomba mesmo sem parada por nível (RENEW não impede o teto na estação).
  - Alvo em outro nó **e** alvo no GPO do gateway, ambos os caminhos.
  - Bomba como zona de grupo hidráulico → sequência de bomba/`max_partidas_hora` respeitada.

---

## Self-Review

**1. Spec coverage:**
- Enchimento automático 1 boia + histerese por tempo → Task 2 (engine minOn/minOff). ✓
- Alvo = zona (outro nó ou gateway) → Task 5 `gwDriveZone` (grupo/direto). ✓
- Fail-safe perda de sinal (desliga+alerta) → Task 2 STALE_STOP + Task 5 alerta/audit. ✓
- Teto 120min + RENEW → Task 2 (`OPEN_CEILING_S`, `RENEW_INTERVAL_MS`). ✓
- Intertravamento ainda veta → Task 5 via `routeZoneToGroup` (checa `zoneVerdict.bloqueada`); zona livre: nota de follow-up abaixo. ✓
- Persistência flash própria → Task 4. ✓
- Config web + UI → Task 3 (codec) + Task 7 (endpoints/aba). ✓
- Backup §5.5 → Task 6. ✓
- Enums append-only → Task 4. ✓
- native-suite-count +2 → Tasks 1, 2. ✓

**2. Placeholder scan:** sem TBD/TODO. Frontend (Task 7) tem código real de exemplo + instrução de seguir `renderGrupos` (CI-only, não native-testável — aceitável).

**3. Type consistency:** `LevelRule`/`LevelControlTable::MAX=4`/`MAGIC=0x494C564C` idênticos entre Tasks 1,3,4,5. `LevelInput{present,active,fresh}` e `LevelIntent{ruleId,zoneId,act,durS}` idênticos entre Tasks 2 e 5. `OPEN_CEILING_S=7200` == `HydraulicGroupEngine::PUMP_CEILING_S`. `buildLevelControls`/`parseLevelUpsert`/`parseLevelDelete` idênticos entre Tasks 3, 6, 7.

**Follow-up conhecido (não bloqueia):** para uma bomba **fora** de grupo hidráulico, o veto de intertravamento não é aplicado no caminho direto de `gwDriveZone` no START (só `routeZoneToGroup` checa). Se o usuário quiser proteção de intertravamento numa bomba avulsa, ou (a) põe a bomba num grupo hidráulico, ou (b) fase futura adiciona o check `zoneVerdict.bloqueada` também no ramo direto de `gwDriveZone`. Registrado como follow-up.
