# Fase 6b — Intertravamentos, fila, log flash e UI (gateway) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fechar a Fase 6 no gateway: motor de intertravamentos global + réplica local na estação (Settings v5), fila de simultaneidade (gate de admissão + FIFO), log de auditoria em flash (~256KB) com export CSV/JSON, janela de manutenção do tamper pelo painel, nomes de sensor lado gateway e a UI do painel.

**Architecture:** Camadas 5a/6a. (A) Puro host-testado — `evalCondition`, `InterlockTable`, `InterlockEngine`, `OpenGate`, `FlashAuditRing`, `SensorNameTable`, Settings v5. (B) Cola no gateway/`IrrigationModule` (decode de sensores no HB, tick do engine, admissão no cronograma, push v5, CMD MAINT, persistência do log). (C) Frontend estático + endpoints CI-only.

**Tech Stack:** C++17, PlatformIO Unity (suíte nativa), `esp32_https_server`, LittleFS, HTML/vanilla-JS.

> Spec de design: `docs/superpowers/specs/2026-07-22-irrigacao-fase6b-gateway-design.md`.

---

## Global Constraints

- Teto absoluto 120 min (`ValveController::MAX_OPEN_SECONDS`) nas válvulas; GPO biestável é a exceção (§8.11).
- Camada A pura: nunca `Arduino.h`/WiFi/HTTP/`FSCom`. Persistência só na camada B.
- Payload rádio ≤ `IrrigationProto::MAX_PAYLOAD` (200 B). **Sem bump de `IrrigationProto::VERSION`** (MAINT entra como novo `MsgType`, retrocompatível).
- Cola HTTP sob `#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER` (padrão 5a/6a; só CI).
- `./bin/run-tests.sh` GREEN. Cada suíte nova bumpa `test/native-suite-count` no MESMO commit.
- `trunk fmt` roda no CI (não instalado localmente). Comentários em português; identificadores no estilo dos vizinhos.

## Como rodar uma suíte nativa (esta máquina precisa de Docker)

```powershell
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f <suite>
```

(Pré-req uma vez: `docker build -f Dockerfile.test -t mesh-test .`.) A corrida filtrada compila o firmware inteiro — erros de cola/módulo aparecem como erro de build. Exit codes: 0 GREEN, 1 RED, 2 AMBER, 3 FILTERED. Ver memória `native-tests-need-docker-on-windows`.

## File Structure

**Criar (camada A pura):**
- `src/modules/irrigation/InterlockEngine.h` / `.cpp` — `evalCondition()` + `InterlockEngine` + `evalLocalInterlocks()` (réplica local, puro).
- `src/modules/irrigation/InterlockTable.h` / `.cpp` — CRUD persistido das regras globais.
- `src/modules/irrigation/OpenGate.h` / `.cpp` — admissão de concorrência + FIFO.
- `src/modules/irrigation/FlashAuditRing.h` / `.cpp` — log circular em flash sobre `ByteStore`.
- `src/modules/irrigation/SensorNameTable.h` / `.cpp` — nomes de sensor lado gateway.
- `test/test_interlock_engine/test_main.cpp`, `test/test_interlock_table/test_main.cpp`, `test/test_open_gate/test_main.cpp`, `test/test_flash_audit_ring/test_main.cpp`, `test/test_sensor_name_table/test_main.cpp`.

**Modificar:**
- `src/modules/irrigation/IrrigationSettings.h` / `.cpp` — ABI v5 (Task 1).
- `src/modules/irrigation/IrrigationProtocol.h` / `.cpp` — `MSG_CMD_MAINT`, `CmdMaint` (Task 15).
- `src/modules/irrigation/StationTelemetryCache.h` / `.cpp` — sensores + tamper (Task 10).
- `src/modules/irrigation/IrrigationGateway.cpp`, `IrrigationModule.h`/`.cpp` — cola (Tasks 11–16).
- `src/modules/irrigation/IrrigationWebApi.h` / `.cpp` — builders/parsers (Task 17).
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — rotas (Task 18).
- `data/irrigacao/index.html`, `app.js`, `style.css` — UI (Task 19).
- `test/test_irrigation_config/test_main.cpp` (v5), `test/test_irrigation_protocol/test_main.cpp` (MAINT), `test/test_web_api/test_main.cpp` (builders/parsers), `test/test_program_scheduler/test_main.cpp` (admissão).
- `test/native-suite-count` — 49 → 54 (uma unidade por suíte nova).
- `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` — Fase 6 concluída (Task 20).

**Códigos compartilhados (fixar cedo, usados em várias tasks):**

```
// condições (InterlockRule.condicao e LocalInterlock.condicao)
enum InterlockCond : uint8_t { COND_ATIVO=0, COND_INATIVO=1, COND_MENOR_QUE=2, COND_MAIOR_QUE=3 };
// ações
enum InterlockAcao : uint8_t { ACAO_BLOQUEAR_ABERTURA=0, ACAO_FECHAR_E_BLOQUEAR=1 };
// tipo de regra global
enum InterlockTipo : uint8_t { IL_SENSOR=0, IL_SIMULTANEIDADE=1 };
```

Estes enums vivem em `InterlockEngine.h` e são reusados por `InterlockTable`, Settings v5 e o frontend.

---

# MARCO 1 — Núcleo de segurança (puro)

## Task 1: `IrrigationSettings` ABI v5 — `LocalInterlock[4]`

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h`, `src/modules/irrigation/IrrigationSettings.cpp`
- Test: `test/test_irrigation_config/test_main.cpp`

Blob 128 → **176 bytes**. Apêndice v5 = `LocalInterlock localInterlocks[4]` (12B cada, 48B). Layout v4 preservado como prefixo (offsets 0–127 inalterados).

- [ ] **Step 1: Escrever teste que falha** — em `test/test_irrigation_config/test_main.cpp`, adicionar:

```cpp
static void test_v5_size_and_offsets()
{
    TEST_ASSERT_EQUAL_UINT32(176, sizeof(IrrigationSettings));
    TEST_ASSERT_EQUAL_UINT32(12, sizeof(IrrigationSettings::LocalInterlock));
    TEST_ASSERT_EQUAL_UINT32(128, offsetof(IrrigationSettings, localInterlocks));
    IrrigationSettings s;
    TEST_ASSERT_EQUAL_UINT16(5, s.version);
}

static void test_v4_blob_migrates_to_v5()
{
    // Um blob v4 (128 B, version=4) migra: prefixo preservado, regras locais zeradas.
    IrrigationSettings v4;
    v4.version = 4;
    v4.numValves = 3;
    v4.sensores[0].pino = 34;
    uint8_t raw[128];
    memcpy(raw, &v4, 128);
    uint16_t ver = 4; memcpy(raw + 4, &ver, 2); // garante version=4 no blob
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, 128, out));
    TEST_ASSERT_EQUAL_UINT16(5, out.version);
    TEST_ASSERT_EQUAL_UINT8(3, out.numValves);
    TEST_ASSERT_EQUAL_INT8(34, out.sensores[0].pino);
    TEST_ASSERT_EQUAL_UINT8(0, out.localInterlocks[0].sensorIdx);
    TEST_ASSERT_EQUAL_UINT8(0, out.localInterlocks[0].saidasMask); // inativo
}
```

Registrar ambas em `setup()` com `RUN_TEST(...)`. (Simplificar o `memcpy` de version no teste: basta `uint16_t v=4; memcpy(raw+4,&v,2);`.)

- [ ] **Step 2: Rodar e verificar RED**

```powershell
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_irrigation_config
```
Esperado: FAIL (`sizeof` == 128, não 176; `localInterlocks` inexistente).

- [ ] **Step 3: Implementar v5 em `IrrigationSettings.h`** — dentro do struct, após `SensorSlot sensores[MAX_SENSORS];`:

```cpp
    // v5 (Fase 6b): réplica local de intertravamentos (§8.10).
    static constexpr uint8_t MAX_LOCAL_INTERLOCKS = 4;
    struct LocalInterlock {
        uint8_t sensorIdx = 0;      // 0..3 (sensor local)
        uint8_t condicao = 0;       // InterlockCond
        uint8_t acao = 0;           // InterlockAcao
        uint8_t saidasMask = 0;     // bits = índices de válvula/GPO locais; 0 = slot inativo
        int32_t valorCenti = 0;
        uint16_t histereseCenti = 0;
        uint16_t pad = 0;           // completa 12 B
    };
    LocalInterlock localInterlocks[MAX_LOCAL_INTERLOCKS];
```

Mudar `uint16_t version = 4;` → `= 5;`. Adicionar constante `static constexpr size_t IRRIGATION_SETTINGS_V4_SIZE = 128;` junto às outras. Atualizar os static_asserts:

```cpp
static_assert(sizeof(IrrigationSettings::LocalInterlock) == 12, "LocalInterlock é ABI on-disk");
static_assert(offsetof(IrrigationSettings, localInterlocks) == 128, "ABI v5");
static_assert(sizeof(IrrigationSettings) == 176, "on-disk settings format is ABI-dependent; bump version on layout change");
```

(Manter os asserts de offset v4 existentes — continuam válidos.)

- [ ] **Step 4: Implementar migração em `IrrigationSettings.cpp`** — no `migrateIrrigationSettings`, o ramo `version == 5` vira o caso exato; `version == 4` passa a migrar:

```cpp
    if (version == 5) {
        if (n != sizeof(IrrigationSettings)) return false;
        memcpy(&out, raw, sizeof(out));
        return true;
    }
    if (version == 4) {
        if (n != IRRIGATION_SETTINGS_V4_SIZE) return false;
        IrrigationSettings s; // defaults v5 (localInterlocks zerados = inativos)
        memcpy(&s, raw, IRRIGATION_SETTINGS_V4_SIZE); // v4 é prefixo do v5
        s.version = 5;
        out = s;
        return true;
    }
```

Nos ramos v2/v3 e v1, trocar `s.version = 4;` → `s.version = 5;`. Em `loadIrrigationSettings`, a linha `bool migrated = (n != sizeof(IrrigationSettings)) || (rawVersion < 4);` vira `... || (rawVersion < 5);`.

- [ ] **Step 5: Rodar e verificar GREEN** (mesmo comando do Step 2). Esperado: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationSettings.h src/modules/irrigation/IrrigationSettings.cpp test/test_irrigation_config/test_main.cpp
git commit -m "feat(irrigation): Settings ABI v5 — réplica local de intertravamentos (fase 6b)"
```

---

## Task 2: `evalCondition()` — avaliação de condição com histerese (puro compartilhado)

**Files:**
- Create: `src/modules/irrigation/InterlockEngine.h`, `src/modules/irrigation/InterlockEngine.cpp`
- Test: `test/test_interlock_engine/test_main.cpp` (nova suíte)

Função pura reusada por gateway (`InterlockEngine`) e estação (réplica local). Latch por regra externo (o chamador guarda o `bool`).

- [ ] **Step 1: Criar header** `InterlockEngine.h`:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

enum InterlockCond : uint8_t { COND_ATIVO = 0, COND_INATIVO = 1, COND_MENOR_QUE = 2, COND_MAIOR_QUE = 3 };
enum InterlockAcao : uint8_t { ACAO_BLOQUEAR_ABERTURA = 0, ACAO_FECHAR_E_BLOQUEAR = 1 };
enum InterlockTipo : uint8_t { IL_SENSOR = 0, IL_SIMULTANEIDADE = 1 };

// Avalia a condição de uma regra sobre uma leitura, com histerese latched.
// `active`/`valueCenti` vêm do sensor (digital usa active; analógico usa valueCenti).
// `latched` é o estado retido pelo chamador (entra e sai por referência).
// Retorna o novo estado de disparo (true = condição satisfeita agora).
bool evalCondition(uint8_t condicao, bool active, int32_t valueCenti,
                   int32_t thresholdCenti, uint16_t histCenti, bool &latched);
```

- [ ] **Step 2: Escrever teste que falha** — `test/test_interlock_engine/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/InterlockEngine.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_digital_ativo()
{
    bool latch = false;
    TEST_ASSERT_TRUE(evalCondition(COND_ATIVO, true, 0, 0, 0, latch));
    TEST_ASSERT_FALSE(evalCondition(COND_ATIVO, false, 0, 0, 0, latch));
}

static void test_analog_menor_que_com_histerese()
{
    // limiar 150 (1,50), histerese 10 (0,10): dispara <150, só desarma >=160.
    bool latch = false;
    TEST_ASSERT_FALSE(evalCondition(COND_MENOR_QUE, false, 200, 150, 10, latch));
    TEST_ASSERT_TRUE(evalCondition(COND_MENOR_QUE, false, 149, 150, 10, latch));  // dispara
    TEST_ASSERT_TRUE(evalCondition(COND_MENOR_QUE, false, 155, 150, 10, latch));  // banda: segue latched
    TEST_ASSERT_FALSE(evalCondition(COND_MENOR_QUE, false, 160, 150, 10, latch)); // desarma
}

static void test_analog_maior_que_com_histerese()
{
    bool latch = false;
    TEST_ASSERT_TRUE(evalCondition(COND_MAIOR_QUE, false, 300, 250, 20, latch));  // >250 dispara
    TEST_ASSERT_TRUE(evalCondition(COND_MAIOR_QUE, false, 235, 250, 20, latch));  // banda [230,250]: latched
    TEST_ASSERT_FALSE(evalCondition(COND_MAIOR_QUE, false, 229, 250, 20, latch)); // <230 desarma
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_digital_ativo);
    RUN_TEST(test_analog_menor_que_com_histerese);
    RUN_TEST(test_analog_maior_que_com_histerese);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Verificar RED**

```powershell
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_interlock_engine
```
Esperado: FAIL (link error — `evalCondition` sem definição).

- [ ] **Step 4: Implementar em `InterlockEngine.cpp`:**

```cpp
#include "modules/irrigation/InterlockEngine.h"

bool evalCondition(uint8_t condicao, bool active, int32_t valueCenti,
                   int32_t thresholdCenti, uint16_t histCenti, bool &latched)
{
    switch (condicao) {
    case COND_ATIVO:   latched = active;  break;
    case COND_INATIVO: latched = !active; break;
    case COND_MENOR_QUE:
        if (valueCenti < thresholdCenti) latched = true;
        else if (valueCenti >= thresholdCenti + (int32_t)histCenti) latched = false;
        break; // dentro da banda: mantém latched
    case COND_MAIOR_QUE:
        if (valueCenti > thresholdCenti) latched = true;
        else if (valueCenti <= thresholdCenti - (int32_t)histCenti) latched = false;
        break;
    default: latched = false; break;
    }
    return latched;
}
```

- [ ] **Step 5: Bump `native-suite-count`** 49 → 50 (nova suíte). Editar `test/native-suite-count` para `50`.

- [ ] **Step 6: Verificar GREEN** (mesmo comando do Step 3). Esperado: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/modules/irrigation/InterlockEngine.h src/modules/irrigation/InterlockEngine.cpp test/test_interlock_engine/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): evalCondition com histerese latched (fase 6b)"
```

---

## Task 3: `InterlockTable` — CRUD persistido das regras globais

**Files:**
- Create: `src/modules/irrigation/InterlockTable.h`, `src/modules/irrigation/InterlockTable.cpp`
- Test: `test/test_interlock_table/test_main.cpp` (nova suíte)

Segue o padrão `ZoneTable` (upsert/removeById/byId/serialize/deserialize, MAGIC+CRC). `MAX=16`.

- [ ] **Step 1: Criar header** `InterlockTable.h`:

```cpp
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
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // falha => tabela vazia

  private:
    InterlockRule rules[MAX];
};
```

- [ ] **Step 2: Teste que falha** — `test/test_interlock_table/test_main.cpp` (espelhar `test_gateway_tables` se existir; senão, este esqueleto):

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/InterlockTable.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_upsert_and_byid()
{
    InterlockTable t;
    InterlockRule r;
    r.id = 3; r.tipo = IL_SENSOR; r.node = 0xA1B2C3D4; r.sensorIdx = 1;
    r.condicao = COND_MENOR_QUE; r.valorCenti = 150; r.acao = ACAO_FECHAR_E_BLOQUEAR;
    r.zoneIds[0] = 2; r.zoneIds[1] = 5;
    TEST_ASSERT_TRUE(t.upsert(r));
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
    const InterlockRule *g = t.byId(3);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4, g->node);
    TEST_ASSERT_EQUAL_UINT8(5, g->zoneIds[1]);
}

static void test_remove_and_full()
{
    InterlockTable t;
    for (uint8_t i = 1; i <= InterlockTable::MAX; i++) {
        InterlockRule r; r.id = i; r.tipo = IL_SIMULTANEIDADE; r.maxAbertas = 2;
        TEST_ASSERT_TRUE(t.upsert(r));
    }
    InterlockRule extra; extra.id = 99;
    TEST_ASSERT_FALSE(t.upsert(extra)); // cheia
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_TRUE(t.upsert(extra));  // agora cabe
}

static void test_roundtrip_serialize()
{
    InterlockTable t;
    InterlockRule r; r.id = 7; r.tipo = IL_SENSOR; r.todas = true;
    r.condicao = COND_ATIVO; r.acao = ACAO_BLOQUEAR_ABERTURA;
    memcpy(r.mensagem, "nivel baixo", 12);
    t.upsert(r);
    uint8_t buf[1024];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    InterlockTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    const InterlockRule *g = t2.byId(7);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_TRUE(g->todas);
    TEST_ASSERT_EQUAL_STRING("nivel baixo", g->mensagem);
}

static void test_corrupt_deserialize_empties()
{
    InterlockTable t;
    uint8_t junk[16] = {0xDE, 0xAD};
    TEST_ASSERT_FALSE(t.deserialize(junk, sizeof(junk)));
    TEST_ASSERT_EQUAL_UINT32(0, t.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_upsert_and_byid);
    RUN_TEST(test_remove_and_full);
    RUN_TEST(test_roundtrip_serialize);
    RUN_TEST(test_corrupt_deserialize_empties);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Verificar RED** — `... -f test_interlock_table`. Esperado: FAIL (link).

- [ ] **Step 4: Implementar `InterlockTable.cpp`** modelando `GatewayTables.cpp` (ler esse arquivo p/ o formato exato de CRC/magic). Estrutura:

```cpp
#include "modules/irrigation/InterlockTable.h"
#include "modules/irrigation/IrrigationProtocol.h" // crc32
#include <string.h>

size_t InterlockTable::count() const {
    size_t c = 0;
    for (auto &r : rules) if (r.id) c++;
    return c;
}
const InterlockRule *InterlockTable::byId(uint8_t id) const {
    if (!id) return nullptr;
    for (auto &r : rules) if (r.id == id) return &r;
    return nullptr;
}
const InterlockRule *InterlockTable::ruleAt(size_t index) const {
    for (auto &r : rules) if (r.id) { if (index == 0) return &r; index--; }
    return nullptr;
}
bool InterlockTable::upsert(const InterlockRule &in) {
    if (!in.id) return false;
    for (auto &r : rules) if (r.id == in.id) { r = in; return true; } // update
    for (auto &r : rules) if (!r.id) { r = in; return true; }         // insert
    return false; // cheia
}
bool InterlockTable::removeById(uint8_t id) {
    for (auto &r : rules) if (r.id == id) { r = InterlockRule{}; return true; }
    return false;
}
// serialize: magic(4) + count(2) + N×sizeof(InterlockRule) + crc32(4).
size_t InterlockTable::serialize(uint8_t *buf, size_t cap) const {
    size_t need = 4 + 2 + count() * sizeof(InterlockRule) + 4;
    if (cap < need) return 0;
    size_t o = 0;
    uint32_t magic = MAGIC; memcpy(buf + o, &magic, 4); o += 4;
    uint16_t c = (uint16_t)count(); memcpy(buf + o, &c, 2); o += 2;
    for (auto &r : rules) if (r.id) { memcpy(buf + o, &r, sizeof(r)); o += sizeof(r); }
    uint32_t crc = IrrigationProto::crc32(buf, o); memcpy(buf + o, &crc, 4); o += 4;
    return o;
}
bool InterlockTable::deserialize(const uint8_t *buf, size_t n) {
    for (auto &r : rules) r = InterlockRule{};
    if (n < 10) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != MAGIC) return false;
    uint16_t c; memcpy(&c, buf + 4, 2);
    size_t need = 4 + 2 + (size_t)c * sizeof(InterlockRule) + 4;
    if (n != need || c > MAX) return false;
    uint32_t crc; memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4)) return false;
    size_t o = 6;
    for (uint16_t i = 0; i < c; i++) { memcpy(&rules[i], buf + o, sizeof(InterlockRule)); o += sizeof(InterlockRule); }
    return true;
}
```

> Nota: `InterlockRule` contém `bool` e `char[]` — copiado por `memcpy` como blob local (não vai ao rádio cru; é só arquivo de flash do gateway). Aceitável, como `Zone`/`StationEntry`.

- [ ] **Step 5: Bump `native-suite-count`** 50 → 51.

- [ ] **Step 6: Verificar GREEN** e **Commit**:

```bash
git add src/modules/irrigation/InterlockTable.h src/modules/irrigation/InterlockTable.cpp test/test_interlock_table/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): InterlockTable — regras globais persistidas (fase 6b)"
```

---

## Task 4: `InterlockEngine` — avaliação global por zona

**Files:**
- Modify: `src/modules/irrigation/InterlockEngine.h` / `.cpp`
- Test: `test/test_interlock_engine/test_main.cpp` (estende)

Avalia todas as regras da `InterlockTable` sobre um snapshot de sensores e devolve, por zona: bloqueada / deve-fechar + id da regra + cap de simultaneidade. Guarda latch por regra (índice na tabela).

- [ ] **Step 1: Estender header** `InterlockEngine.h` — adicionar após `evalCondition`:

```cpp
#include "modules/irrigation/InterlockTable.h"

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
    bool latched[InterlockTable::MAX] = {false};
    // resultado do último evaluate por índice de regra
    bool fired[InterlockTable::MAX] = {false};
    const InterlockTable *lastTbl = nullptr;
    static const SensorSnapshot *findSnap(const SensorSnapshot *s, size_t n, uint32_t node, uint8_t idx);
    static bool ruleCoversZone(const InterlockRule &r, uint8_t zoneId);
};
```

- [ ] **Step 2: Testes que falham** — adicionar a `test_interlock_engine/test_main.cpp`:

```cpp
#include "modules/irrigation/InterlockTable.h"

static void test_engine_fecha_e_bloqueia_zona()
{
    InterlockTable tbl;
    InterlockRule r; r.id = 1; r.tipo = IL_SENSOR; r.node = 0xAA; r.sensorIdx = 0;
    r.condicao = COND_MENOR_QUE; r.valorCenti = 150; r.histereseCenti = 10;
    r.acao = ACAO_FECHAR_E_BLOQUEAR; r.zoneIds[0] = 4;
    tbl.upsert(r);
    InterlockEngine eng;
    SensorSnapshot s{0xAA, 0, true, false, 149};
    eng.evaluate(tbl, &s, 1);
    ZoneVerdict v = eng.zoneVerdict(4);
    TEST_ASSERT_TRUE(v.deveFechar);
    TEST_ASSERT_TRUE(v.bloqueada);
    TEST_ASSERT_EQUAL_UINT8(1, v.ruleId);
    TEST_ASSERT_FALSE(eng.zoneVerdict(9).bloqueada); // zona não coberta
}

static void test_engine_todas_zonas_e_bloquear_abertura()
{
    InterlockTable tbl;
    InterlockRule r; r.id = 2; r.tipo = IL_SENSOR; r.node = 0xBB; r.sensorIdx = 1;
    r.condicao = COND_ATIVO; r.acao = ACAO_BLOQUEAR_ABERTURA; r.todas = true;
    tbl.upsert(r);
    InterlockEngine eng;
    SensorSnapshot s{0xBB, 1, true, true, 0};
    eng.evaluate(tbl, &s, 1);
    ZoneVerdict v = eng.zoneVerdict(123);
    TEST_ASSERT_TRUE(v.bloqueada);
    TEST_ASSERT_FALSE(v.deveFechar); // bloquear_abertura não fecha o que já está aberto
}

static void test_engine_cap_simultaneidade()
{
    InterlockTable tbl;
    InterlockRule r; r.id = 3; r.tipo = IL_SIMULTANEIDADE; r.maxAbertas = 2;
    tbl.upsert(r);
    InterlockEngine eng;
    TEST_ASSERT_EQUAL_UINT8(2, eng.evaluate(tbl, nullptr, 0));
}

static void test_engine_sensor_ausente_nao_dispara()
{
    InterlockTable tbl;
    InterlockRule r; r.id = 1; r.tipo = IL_SENSOR; r.node = 0xAA; r.sensorIdx = 0;
    r.condicao = COND_ATIVO; r.acao = ACAO_FECHAR_E_BLOQUEAR; r.todas = true;
    tbl.upsert(r);
    InterlockEngine eng;
    SensorSnapshot s{0xAA, 0, false, true, 0}; // present=false
    eng.evaluate(tbl, &s, 1);
    TEST_ASSERT_FALSE(eng.zoneVerdict(1).deveFechar);
}
```

Registrar os quatro em `setup()`.

- [ ] **Step 3: Verificar RED** — `... -f test_interlock_engine`. Esperado: FAIL.

- [ ] **Step 4: Implementar em `InterlockEngine.cpp`:**

```cpp
const SensorSnapshot *InterlockEngine::findSnap(const SensorSnapshot *s, size_t n, uint32_t node, uint8_t idx) {
    for (size_t i = 0; i < n; i++) if (s[i].node == node && s[i].sensorIdx == idx) return &s[i];
    return nullptr;
}
bool InterlockEngine::ruleCoversZone(const InterlockRule &r, uint8_t zoneId) {
    if (r.todas) return true;
    for (uint8_t z : r.zoneIds) { if (z == 0) break; if (z == zoneId) return true; }
    return false;
}
uint8_t InterlockEngine::evaluate(const InterlockTable &tbl, const SensorSnapshot *snaps, size_t nSnaps) {
    lastTbl = &tbl;
    uint8_t cap = 0; // 0 = sem limite
    for (size_t i = 0; i < InterlockTable::MAX; i++) {
        const InterlockRule *r = tbl.ruleAt(i);
        fired[i] = false;
        if (!r) { latched[i] = false; continue; }
        if (r->tipo == IL_SIMULTANEIDADE) {
            if (r->maxAbertas > 0 && (cap == 0 || r->maxAbertas < cap)) cap = r->maxAbertas;
            continue;
        }
        const SensorSnapshot *sn = findSnap(snaps, nSnaps, r->node, r->sensorIdx);
        if (!sn || !sn->present) { latched[i] = false; continue; } // sem dado = não dispara
        fired[i] = evalCondition(r->condicao, sn->active, sn->valueCenti,
                                 r->valorCenti, r->histereseCenti, latched[i]);
    }
    return cap;
}
ZoneVerdict InterlockEngine::zoneVerdict(uint8_t zoneId) const {
    ZoneVerdict v;
    if (!lastTbl) return v;
    for (size_t i = 0; i < InterlockTable::MAX; i++) {
        if (!fired[i]) continue;
        const InterlockRule *r = lastTbl->ruleAt(i);
        if (!r || r->tipo != IL_SENSOR) continue;
        if (!ruleCoversZone(*r, zoneId)) continue;
        v.bloqueada = true; v.ruleId = r->id;
        if (r->acao == ACAO_FECHAR_E_BLOQUEAR) v.deveFechar = true;
    }
    return v;
}
```

> `ruleAt(i)` devolve a i-ésima **ocupada**, então `fired[]`/`latched[]` são indexados pela posição ocupada, não pelo slot cru. Consistente entre `evaluate` e `zoneVerdict` (ambos iteram `ruleAt(i)`). Cuidado: se a tabela mudar entre ticks, o latch reindexado é aceitável (reconstrói em 1 tick).

- [ ] **Step 5: Verificar GREEN** e **Commit**:

```bash
git add src/modules/irrigation/InterlockEngine.h src/modules/irrigation/InterlockEngine.cpp test/test_interlock_engine/test_main.cpp
git commit -m "feat(irrigation): InterlockEngine — veredito global por zona (fase 6b)"
```

---

## Task 5: `evalLocalInterlocks()` — réplica local (puro)

**Files:**
- Modify: `src/modules/irrigation/InterlockEngine.h` / `.cpp`
- Test: `test/test_interlock_engine/test_main.cpp` (estende)

Avaliação da réplica local: recebe as `LocalInterlock[4]` (de Settings v5) + leituras locais → devolve máscara de saídas a fechar+bloquear e a bloquear. Latch por slot (chamador guarda).

- [ ] **Step 1: Estender header** — em `InterlockEngine.h`:

```cpp
#include "modules/irrigation/IrrigationSettings.h"

struct LocalReplicaOut {
    uint8_t fecharMask = 0;   // saídas a fechar+bloquear agora
    uint8_t bloquearMask = 0; // saídas com abertura bloqueada
};

// Avalia as regras locais. `readings`/`nReadings`: leituras do SensorSampler local
// (id = sensorIdx). `latched[4]`: estado retido pelo chamador. Puro.
LocalReplicaOut evalLocalInterlocks(const IrrigationSettings::LocalInterlock *rules, size_t nRules,
                                    const IrrigationProto::SensorReading *readings, size_t nReadings,
                                    bool *latched);
```

- [ ] **Step 2: Testes que falham** — adicionar:

```cpp
static void test_local_replica_fecha_e_bloqueia()
{
    IrrigationSettings::LocalInterlock rules[4] = {};
    rules[0].sensorIdx = 0; rules[0].condicao = COND_MAIOR_QUE; rules[0].valorCenti = 300;
    rules[0].histereseCenti = 20; rules[0].acao = ACAO_FECHAR_E_BLOQUEAR; rules[0].saidasMask = 0b0000'0010;
    IrrigationProto::SensorReading rd[1] = {{0, 1, 0}};
    rd[0].id = 0; rd[0].tipo = 1; rd[0].valueCenti = 350; // > 300 → dispara
    bool latch[4] = {false};
    LocalReplicaOut o = evalLocalInterlocks(rules, 4, rd, 1, latch);
    TEST_ASSERT_EQUAL_UINT8(0b0000'0010, o.fecharMask);
    TEST_ASSERT_EQUAL_UINT8(0b0000'0010, o.bloquearMask);
}

static void test_local_replica_slot_inativo_ignorado()
{
    IrrigationSettings::LocalInterlock rules[4] = {}; // saidasMask=0 → inativo
    IrrigationProto::SensorReading rd[1] = {};
    bool latch[4] = {false};
    LocalReplicaOut o = evalLocalInterlocks(rules, 4, rd, 1, latch);
    TEST_ASSERT_EQUAL_UINT8(0, o.fecharMask);
    TEST_ASSERT_EQUAL_UINT8(0, o.bloquearMask);
}
```

- [ ] **Step 3: Verificar RED** — `... -f test_interlock_engine`.

- [ ] **Step 4: Implementar em `InterlockEngine.cpp`:**

```cpp
LocalReplicaOut evalLocalInterlocks(const IrrigationSettings::LocalInterlock *rules, size_t nRules,
                                    const IrrigationProto::SensorReading *readings, size_t nReadings,
                                    bool *latched)
{
    LocalReplicaOut out;
    for (size_t i = 0; i < nRules; i++) {
        const auto &r = rules[i];
        if (r.saidasMask == 0) { if (latched) latched[i] = false; continue; } // inativo
        const IrrigationProto::SensorReading *rd = nullptr;
        for (size_t k = 0; k < nReadings; k++) if (readings[k].id == r.sensorIdx) { rd = &readings[k]; break; }
        if (!rd) { if (latched) latched[i] = false; continue; }
        bool active = rd->valueCenti != 0; // digital: 0/100
        bool fire = evalCondition(r.condicao, active, rd->valueCenti,
                                  r.valorCenti, r.histereseCenti, latched[i]);
        if (fire) {
            out.bloquearMask |= r.saidasMask;
            if (r.acao == ACAO_FECHAR_E_BLOQUEAR) out.fecharMask |= r.saidasMask;
        }
    }
    return out;
}
```

- [ ] **Step 5: Verificar GREEN** e **Commit**:

```bash
git add src/modules/irrigation/InterlockEngine.h src/modules/irrigation/InterlockEngine.cpp test/test_interlock_engine/test_main.cpp
git commit -m "feat(irrigation): evalLocalInterlocks — réplica local sem rádio (fase 6b)"
```

---

## Task 6: `OpenGate` — admissão de concorrência + FIFO

**Files:**
- Create: `src/modules/irrigation/OpenGate.h`, `src/modules/irrigation/OpenGate.cpp`
- Test: `test/test_open_gate/test_main.cpp` (nova suíte)

- [ ] **Step 1: Criar header** `OpenGate.h`:

```cpp
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

  private:
    uint8_t open[MAX_OPEN] = {0};
    Pending queue[MAX_QUEUE] = {};
    size_t qHead = 0, qTail = 0, qSize = 0;
    uint8_t capOpen = 0;
    bool hasCapacity() const { return capOpen == 0 || openCount() < capOpen; }
    bool markOpen(uint8_t zoneId);
};
```

- [ ] **Step 2: Teste que falha** — `test/test_open_gate/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/OpenGate.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_admite_ate_cap_depois_enfileira()
{
    OpenGate g; g.setCap(2);
    TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(1, 60));
    TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(2, 60));
    TEST_ASSERT_EQUAL(OpenGate::Decision::HOLD, g.request(3, 90)); // cheio → fila
    TEST_ASSERT_TRUE(g.isQueued(3));
    TEST_ASSERT_EQUAL_UINT32(2, g.openCount());
}

static void test_release_libera_fila_em_ordem()
{
    OpenGate g; g.setCap(1);
    g.request(1, 60);
    g.request(2, 70); // fila
    g.request(3, 80); // fila
    TEST_ASSERT_EQUAL_UINT8(0, g.nextAdmittable().zoneId); // sem capacidade ainda
    g.release(1);
    OpenGate::Pending p = g.nextAdmittable();
    TEST_ASSERT_EQUAL_UINT8(2, p.zoneId); // FIFO: 2 antes de 3
    TEST_ASSERT_EQUAL_UINT16(70, p.durationS); // carrega a duração enfileirada
    TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(2, 70)); // glue readmite
    g.release(2);
    TEST_ASSERT_EQUAL_UINT8(3, g.nextAdmittable().zoneId);
}

static void test_cap_zero_sem_limite()
{
    OpenGate g; g.setCap(0);
    for (uint8_t i = 1; i <= 10; i++)
        TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(i, 60));
    TEST_ASSERT_EQUAL_UINT32(10, g.openCount());
}

static void test_request_zona_ja_aberta_idempotente()
{
    OpenGate g; g.setCap(2);
    g.request(1, 60);
    TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(1, 60)); // renovação, não conta 2×
    TEST_ASSERT_EQUAL_UINT32(1, g.openCount());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_admite_ate_cap_depois_enfileira);
    RUN_TEST(test_release_libera_fila_em_ordem);
    RUN_TEST(test_cap_zero_sem_limite);
    RUN_TEST(test_request_zona_ja_aberta_idempotente);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Verificar RED** — `... -f test_open_gate`.

- [ ] **Step 4: Implementar `OpenGate.cpp`:**

```cpp
#include "modules/irrigation/OpenGate.h"

size_t OpenGate::openCount() const {
    size_t c = 0;
    for (uint8_t z : open) if (z) c++;
    return c;
}
bool OpenGate::markOpen(uint8_t zoneId) {
    for (uint8_t z : open) if (z == zoneId) return true; // já aberta
    for (uint8_t &z : open) if (!z) { z = zoneId; return true; }
    return false; // sem slot (não deveria: MAX_OPEN = ZoneTable::MAX)
}
bool OpenGate::isQueued(uint8_t zoneId) const {
    for (size_t i = 0; i < qSize; i++) if (queue[(qHead + i) % MAX_QUEUE].zoneId == zoneId) return true;
    return false;
}
OpenGate::Decision OpenGate::request(uint8_t zoneId, uint16_t durationS) {
    for (uint8_t z : open) if (z == zoneId) return Decision::ADMIT; // renovação idempotente
    if (hasCapacity()) { markOpen(zoneId); return Decision::ADMIT; }
    if (!isQueued(zoneId) && qSize < MAX_QUEUE) {
        queue[qTail] = { zoneId, durationS }; qTail = (qTail + 1) % MAX_QUEUE; qSize++;
    }
    return Decision::HOLD;
}
void OpenGate::release(uint8_t zoneId) {
    for (uint8_t &z : open) if (z == zoneId) { z = 0; return; }
}
OpenGate::Pending OpenGate::nextAdmittable() {
    if (qSize == 0 || !hasCapacity()) return {};
    Pending p = queue[qHead]; qHead = (qHead + 1) % MAX_QUEUE; qSize--;
    return p; // glue chama request(p.zoneId, p.durationS) em seguida p/ registrar a abertura
}
```

- [ ] **Step 5: Bump `native-suite-count`** 51 → 52. **Verificar GREEN** e **Commit**:

```bash
git add src/modules/irrigation/OpenGate.h src/modules/irrigation/OpenGate.cpp test/test_open_gate/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): OpenGate — admissão de simultaneidade + FIFO (fase 6b)"
```

---

# MARCO 2 — Log do gateway + telemetria (puro)

## Task 7: `FlashAuditRing` — ring circular em flash (append + iteração)

**Files:**
- Create: `src/modules/irrigation/FlashAuditRing.h`, `src/modules/irrigation/FlashAuditRing.cpp`
- Test: `test/test_flash_audit_ring/test_main.cpp` (nova suíte)

Ring sobre `ByteStore` abstrato (fake em memória nos testes). Reusa `AuditRecord` (16B) da 6a.

- [ ] **Step 1: Criar header** `FlashAuditRing.h`:

```cpp
#pragma once
#include "modules/irrigation/AuditLog.h" // AuditRecord
#include <stddef.h>
#include <stdint.h>

// Abstrai o armazenamento de bytes (arquivo LittleFS no gateway; vetor nos testes).
class IByteStore {
  public:
    virtual ~IByteStore() = default;
    virtual size_t size() const = 0;
    virtual bool read(size_t off, void *dst, size_t n) const = 0;
    virtual bool write(size_t off, const void *src, size_t n) = 0;
};

// Layout: header(16B) + N×16B slots. header = magic(4) head(4) count(4) crc(4).
// `head` = índice do próximo slot a escrever; `count` satura em capacidade.
class FlashAuditRing {
  public:
    static constexpr uint32_t MAGIC = 0x49415232; // "IAR2"
    static constexpr size_t HEADER = 16;
    static constexpr size_t REC = sizeof(AuditRecord); // 16

    explicit FlashAuditRing(IByteStore &store) : store(store) {}
    bool begin();                 // lê/valida header; se inválido, formata (zera)
    void append(const AuditRecord &r);
    size_t count() const { return num; }
    size_t capacity() const { return (store.size() >= HEADER + REC) ? (store.size() - HEADER) / REC : 0; }
    bool at(size_t i, AuditRecord &out) const; // i=0 = mais recente
    void clear();

  private:
    IByteStore &store;
    uint32_t head = 0;
    uint32_t num = 0;
    void writeHeader();
    size_t slotOffset(uint32_t idx) const { return HEADER + (idx % capacity()) * REC; }
};
```

- [ ] **Step 2: Teste que falha** — `test/test_flash_audit_ring/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/FlashAuditRing.h"
#include <string.h>
#include <unity.h>
#include <vector>

struct MemStore : IByteStore {
    std::vector<uint8_t> bytes;
    explicit MemStore(size_t n) : bytes(n, 0) {}
    size_t size() const override { return bytes.size(); }
    bool read(size_t off, void *dst, size_t n) const override {
        if (off + n > bytes.size()) return false;
        memcpy(dst, bytes.data() + off, n); return true;
    }
    bool write(size_t off, const void *src, size_t n) override {
        if (off + n > bytes.size()) return false;
        memcpy(bytes.data() + off, src, n); return true;
    }
};

static AuditRecord rec(uint32_t ts) { AuditRecord r; r.tsSecs = ts; r.action = 1; return r; }

void setUp(void) {}
void tearDown(void) {}

static void test_append_and_newest_first()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC); // cap 4
    FlashAuditRing ring(s);
    TEST_ASSERT_TRUE(ring.begin());
    ring.append(rec(10)); ring.append(rec(20)); ring.append(rec(30));
    TEST_ASSERT_EQUAL_UINT32(3, ring.count());
    AuditRecord g;
    TEST_ASSERT_TRUE(ring.at(0, g)); TEST_ASSERT_EQUAL_UINT32(30, g.tsSecs); // mais recente
    TEST_ASSERT_TRUE(ring.at(2, g)); TEST_ASSERT_EQUAL_UINT32(10, g.tsSecs);
}

static void test_wrap_keeps_recent()
{
    MemStore s(FlashAuditRing::HEADER + 3 * FlashAuditRing::REC); // cap 3
    FlashAuditRing ring(s); ring.begin();
    for (uint32_t i = 1; i <= 5; i++) ring.append(rec(i * 10)); // 10..50, cap 3
    TEST_ASSERT_EQUAL_UINT32(3, ring.count());
    AuditRecord g;
    ring.at(0, g); TEST_ASSERT_EQUAL_UINT32(50, g.tsSecs);
    ring.at(2, g); TEST_ASSERT_EQUAL_UINT32(30, g.tsSecs); // 10 e 20 perdidos
}

static void test_persists_across_reopen()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    { FlashAuditRing ring(s); ring.begin(); ring.append(rec(77)); }
    FlashAuditRing ring2(s);
    TEST_ASSERT_TRUE(ring2.begin()); // header válido
    TEST_ASSERT_EQUAL_UINT32(1, ring2.count());
    AuditRecord g; ring2.at(0, g); TEST_ASSERT_EQUAL_UINT32(77, g.tsSecs);
}

static void test_corrupt_header_formats()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    s.bytes[0] = 0xFF; // magic inválido
    FlashAuditRing ring(s);
    TEST_ASSERT_TRUE(ring.begin()); // formata em vez de falhar
    TEST_ASSERT_EQUAL_UINT32(0, ring.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_append_and_newest_first);
    RUN_TEST(test_wrap_keeps_recent);
    RUN_TEST(test_persists_across_reopen);
    RUN_TEST(test_corrupt_header_formats);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Verificar RED** — `... -f test_flash_audit_ring`.

- [ ] **Step 4: Implementar `FlashAuditRing.cpp`:**

```cpp
#include "modules/irrigation/FlashAuditRing.h"
#include "modules/irrigation/IrrigationProtocol.h" // crc32
#include <string.h>

bool FlashAuditRing::begin() {
    if (capacity() == 0) { head = num = 0; return false; }
    uint8_t hdr[HEADER];
    if (!store.read(0, hdr, HEADER)) { clear(); return true; }
    uint32_t magic, h, c, crc;
    memcpy(&magic, hdr, 4); memcpy(&h, hdr + 4, 4); memcpy(&c, hdr + 8, 4); memcpy(&crc, hdr + 12, 4);
    if (magic != MAGIC || crc != IrrigationProto::crc32(hdr, 12) || c > capacity() || h >= capacity()) {
        clear(); return true; // formata
    }
    head = h; num = c; return true;
}
void FlashAuditRing::writeHeader() {
    uint8_t hdr[HEADER];
    uint32_t magic = MAGIC;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &head, 4); memcpy(hdr + 8, &num, 4);
    uint32_t crc = IrrigationProto::crc32(hdr, 12); memcpy(hdr + 12, &crc, 4);
    store.write(0, hdr, HEADER);
}
void FlashAuditRing::clear() {
    head = num = 0; writeHeader();
}
void FlashAuditRing::append(const AuditRecord &r) {
    if (capacity() == 0) return;
    store.write(slotOffset(head), &r, REC);
    head = (head + 1) % capacity();
    if (num < capacity()) num++;
    writeHeader();
}
bool FlashAuditRing::at(size_t i, AuditRecord &out) const {
    if (i >= num) return false;
    // i=0 = mais recente = head-1
    uint32_t idx = (head + capacity() - 1 - (uint32_t)i) % capacity();
    return store.read(slotOffset(idx), &out, REC);
}
```

- [ ] **Step 5: Bump `native-suite-count`** 52 → 53. **Verificar GREEN** e **Commit**:

```bash
git add src/modules/irrigation/FlashAuditRing.h src/modules/irrigation/FlashAuditRing.cpp test/test_flash_audit_ring/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): FlashAuditRing — log circular em flash (fase 6b)"
```

---

## Task 8: Export CSV/JSON do log (puro, streaming)

**Files:**
- Modify: `src/modules/irrigation/FlashAuditRing.h` / `.cpp`
- Test: `test/test_flash_audit_ring/test_main.cpp` (estende)

Formata o log num buffer do chamador, do mais recente ao mais antigo, com corte por capacidade do buffer (streaming — não materializa tudo). Rótulos de origem/ação vêm de tabelas estáticas (contrato de 3 vias, ver `AuditLog.h`).

- [ ] **Step 1: Estender header** — em `FlashAuditRing.h`:

```cpp
    // Escreve até `cap` bytes. Retorna bytes escritos (trunca no último registro
    // que couber inteiro). `fromNewest` limita a quantos registros no máximo.
    size_t toCsv(char *buf, size_t cap, size_t maxRecords) const;
    size_t toJson(char *buf, size_t cap, size_t maxRecords) const;
```

- [ ] **Step 2: Testes que falham** — adicionar:

```cpp
static void test_csv_header_and_rows()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    FlashAuditRing ring(s); ring.begin();
    AuditRecord r; r.tsSecs = 100; r.origin = 1; r.action = 0; r.target = 3;
    r.result = 0; r.node = 0xABCD; r.seq = 9;
    ring.append(r);
    char out[512];
    size_t n = ring.toCsv(out, sizeof(out), 50);
    TEST_ASSERT_GREATER_THAN(0, n);
    out[n] = 0;
    TEST_ASSERT_NOT_NULL(strstr(out, "ts,origem,acao,alvo,resultado,node,seq")); // cabeçalho
    TEST_ASSERT_NOT_NULL(strstr(out, "100,"));
}

static void test_json_array()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    FlashAuditRing ring(s); ring.begin();
    ring.append(rec(42));
    char out[512];
    size_t n = ring.toJson(out, sizeof(out), 50);
    out[n] = 0;
    TEST_ASSERT_EQUAL_CHAR('[', out[0]);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"ts\":42"));
}
```

- [ ] **Step 3: Verificar RED**. **Step 4: Implementar** em `.cpp` (usar `snprintf` por registro, parar quando o próximo não couber):

```cpp
size_t FlashAuditRing::toCsv(char *buf, size_t cap, size_t maxRecords) const {
    size_t o = 0;
    int w = snprintf(buf + o, cap - o, "ts,origem,acao,alvo,resultado,node,seq\n");
    if (w < 0 || (size_t)w >= cap - o) return o; o += w;
    size_t lim = num < maxRecords ? num : maxRecords;
    for (size_t i = 0; i < lim; i++) {
        AuditRecord r; if (!at(i, r)) break;
        char line[96];
        int lw = snprintf(line, sizeof(line), "%u,%u,%u,%u,%u,%08x,%u\n",
                          (unsigned)r.tsSecs, r.origin, r.action, r.target, r.result,
                          (unsigned)r.node, (unsigned)r.seq);
        if (lw < 0 || o + (size_t)lw >= cap) break; // trunca no que couber inteiro
        memcpy(buf + o, line, lw); o += lw;
    }
    return o;
}
size_t FlashAuditRing::toJson(char *buf, size_t cap, size_t maxRecords) const {
    size_t o = 0;
    if (cap < 2) return 0;
    buf[o++] = '[';
    size_t lim = num < maxRecords ? num : maxRecords;
    for (size_t i = 0; i < lim; i++) {
        AuditRecord r; if (!at(i, r)) break;
        char item[128];
        int lw = snprintf(item, sizeof(item),
            "%s{\"ts\":%u,\"origem\":%u,\"acao\":%u,\"alvo\":%u,\"resultado\":%u,\"node\":%u,\"seq\":%u}",
            i ? "," : "", (unsigned)r.tsSecs, r.origin, r.action, r.target, r.result,
            (unsigned)r.node, (unsigned)r.seq);
        if (lw < 0 || o + (size_t)lw + 1 >= cap) break; // +1 p/ o ']'
        memcpy(buf + o, item, lw); o += lw;
    }
    buf[o++] = ']';
    return o;
}
```

- [ ] **Step 5: Verificar GREEN** e **Commit**:

```bash
git add src/modules/irrigation/FlashAuditRing.h src/modules/irrigation/FlashAuditRing.cpp test/test_flash_audit_ring/test_main.cpp
git commit -m "feat(irrigation): export CSV/JSON do log de auditoria (fase 6b)"
```

---

## Task 9: `SensorNameTable` — nomes de sensor lado gateway

**Files:**
- Create: `src/modules/irrigation/SensorNameTable.h`, `src/modules/irrigation/SensorNameTable.cpp`
- Test: `test/test_sensor_name_table/test_main.cpp` (nova suíte)

- [ ] **Step 1: Criar header** `SensorNameTable.h`:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

struct SensorName {
    uint32_t node = 0;      // 0 = slot vazio
    uint8_t sensorIdx = 0;
    char name[16] = {0};
};

class SensorNameTable {
  public:
    static constexpr size_t MAX = 32;
    static constexpr uint32_t MAGIC = 0x49534E31; // "ISN1"
    bool set(uint32_t node, uint8_t sensorIdx, const char *name); // upsert; false = cheia
    const char *get(uint32_t node, uint8_t sensorIdx) const;      // nullptr = sem nome
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);

  private:
    SensorName names[MAX];
};
```

- [ ] **Step 2: Teste que falha** — `test/test_sensor_name_table/test_main.cpp` (mesma forma das outras suítes; casos: set/get, update sobrescreve, cheia→false, roundtrip serialize, deserialize corrompido→vazio):

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/SensorNameTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_set_get_update()
{
    SensorNameTable t;
    TEST_ASSERT_TRUE(t.set(0xAA, 1, "pressao"));
    TEST_ASSERT_EQUAL_STRING("pressao", t.get(0xAA, 1));
    TEST_ASSERT_NULL(t.get(0xAA, 2));
    TEST_ASSERT_TRUE(t.set(0xAA, 1, "pressao linha")); // update
    TEST_ASSERT_EQUAL_STRING("pressao linha", t.get(0xAA, 1));
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
}

static void test_roundtrip()
{
    SensorNameTable t; t.set(0xBB, 0, "nivel");
    uint8_t buf[2048];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    SensorNameTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_STRING("nivel", t2.get(0xBB, 0));
}

static void test_corrupt_empties()
{
    SensorNameTable t;
    uint8_t junk[8] = {1, 2, 3};
    TEST_ASSERT_FALSE(t.deserialize(junk, sizeof(junk)));
    TEST_ASSERT_EQUAL_UINT32(0, t.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_set_get_update);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_corrupt_empties);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Verificar RED** — `... -f test_sensor_name_table`.

- [ ] **Step 4: Implementar `SensorNameTable.cpp`** (mesmo padrão magic+count+records+crc de `InterlockTable`; `set` faz `strncpy(name, in, 15)` e garante `\0`). Modelar em `InterlockTable::serialize/deserialize` da Task 3.

- [ ] **Step 5: Bump `native-suite-count`** 53 → 54. **Verificar GREEN** e **Commit**:

```bash
git add src/modules/irrigation/SensorNameTable.h src/modules/irrigation/SensorNameTable.cpp test/test_sensor_name_table/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): SensorNameTable — nomes de sensor no gateway (fase 6b)"
```

---

## Task 10: `StationTelemetryCache` — sensores + tamper no cache

**Files:**
- Modify: `src/modules/irrigation/StationTelemetryCache.h` / `.cpp`
- Test: `test/test_station_telemetry/test_main.cpp` se existir; senão adicionar casos onde `StationTelemetryCache` já é testado (procurar com `grep -rl StationTelemetryCache test/`). Se não houver suíte, criar `test/test_station_telemetry` e bumpar count 54 → 55.

- [ ] **Step 1: Estender `StationTelemetry`** — em `StationTelemetryCache.h`, adicionar campos:

```cpp
#include "modules/irrigation/IrrigationProtocol.h" // SensorReading, HB_MAX_SENSORS

struct StationTelemetry {
    uint32_t node = 0;
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0;
    int8_t snrQuarterDb = 0;
    uint16_t rebootCount = 0;
    uint8_t flags = 0;
    uint32_t configEpoch = 0;
    uint32_t atMs = 0;
    // Fase 6b: bloco de sensores do heartbeat (feeds InterlockEngine).
    uint8_t sensorCount = 0;
    IrrigationProto::SensorReading sensors[IrrigationProto::HB_MAX_SENSORS] = {};
    bool tamper = false; // HB_FLAG_TAMPER extraído
};
```

Também adicionar ao `StationTelemetryCache` (público) um getter por índice, usado pelo tick da Task 12:

```cpp
    const StationTelemetry *entryAt(size_t i) const { return i < MAX ? &entries[i] : nullptr; }
```

- [ ] **Step 2: Teste que falha** — verificar que `update()` preserva os novos campos e `byNode` os devolve:

```cpp
static void test_cache_guarda_sensores_e_tamper()
{
    StationTelemetryCache c;
    StationTelemetry t; t.node = 0xAA; t.sensorCount = 2; t.tamper = true;
    t.sensors[0].id = 0; t.sensors[0].tipo = 1; t.sensors[0].valueCenti = 250;
    c.update(t);
    const StationTelemetry *g = c.byNode(0xAA);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(2, g->sensorCount);
    TEST_ASSERT_TRUE(g->tamper);
    TEST_ASSERT_EQUAL_INT16(250, g->sensors[0].valueCenti);
}
```

- [ ] **Step 3–5:** Como `update()` já faz cópia do struct por `=`, o teste deve passar após só adicionar os campos (o `.cpp` pode não precisar mudar — confirmar). **Verificar GREEN** e **Commit**:

```bash
git add src/modules/irrigation/StationTelemetryCache.h test/test_station_telemetry/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): telemetria guarda sensores + tamper (fase 6b)"
```

---

# MARCO 3 — Cola do gateway (CI/build; não roda na suíte nativa pura)

> As Tasks 11–16 tocam `IrrigationGateway.cpp`/`IrrigationModule.cpp`. Não há teste unitário nativo direto (dependem de rádio/RTC/FS). A verificação é: a corrida filtrada de QUALQUER suíte compila o firmware inteiro — erros de cola aparecem como **erro de build**. Rodar `-f test_interlock_engine` após cada uma. Revisar à mão a lógica.

## Task 11: Decodificar sensores do heartbeat para o cache

**Files:** Modify `src/modules/irrigation/IrrigationGateway.cpp` (handler de `MSG_HEARTBEAT`).

- [ ] **Step 1:** Localizar onde o gateway trata `decodeHeartbeat` e popula `StationTelemetry` (grep `decodeHeartbeat` em `IrrigationGateway.cpp`). Após montar o `StationTelemetry t`, copiar o bloco de sensores e o bit de tamper:

```cpp
    t.sensorCount = hb.sensorCount;
    for (uint8_t i = 0; i < hb.sensorCount && i < IrrigationProto::HB_MAX_SENSORS; i++)
        t.sensors[i] = hb.sensors[i];
    t.tamper = (hb.flags & IrrigationProto::HB_FLAG_TAMPER) != 0;
    telemetryCache.update(t);
```

- [ ] **Step 2:** Compilar via `-f test_interlock_engine`. Esperado: GREEN (build ok).
- [ ] **Step 3: Commit** `feat(irrigation): gateway decodifica sensores do heartbeat (fase 6b)`.

## Task 12: Tick do `InterlockEngine` no `runOnce` do gateway

**Files:** Modify `IrrigationGateway.h`/`.cpp`.

- [ ] **Step 1:** Adicionar membros ao gateway: `InterlockTable interlocks;`, `InterlockEngine interlockEngine;`, `OpenGate openGate;`. Carregar/salvar `interlocks` em `/prefs/irrigation_interlocks.dat` (padrão de persistência das outras tabelas — modelar em como `ZoneTable` é carregada).

- [ ] **Step 2:** No `runOnce` (1×/s), montar o snapshot a partir do `telemetryCache` e avaliar:

```cpp
    SensorSnapshot snaps[StationTelemetryCache::MAX * IrrigationProto::HB_MAX_SENSORS];
    size_t ns = 0;
    for (size_t i = 0; i < StationTelemetryCache::MAX; i++) {
        const StationTelemetry *t = telemetryCache.entryAt(i); // adicionar getter se faltar
        if (!t || !t->node) continue;
        for (uint8_t k = 0; k < t->sensorCount; k++) {
            snaps[ns++] = SensorSnapshot{ t->node, t->sensors[k].id, true,
                                          t->sensors[k].valueCenti != 0, t->sensors[k].valueCenti };
        }
    }
    uint8_t cap = interlockEngine.evaluate(interlocks, snaps, ns);
    openGate.setCap(cap);
    // fechar_e_bloquear: para cada zona aberta com deveFechar, fechar por rádio + auditar
    for (size_t i = 0; i < zones.count(); i++) {
        const Zone *z = zones.zoneAt(i);
        ZoneVerdict v = interlockEngine.zoneVerdict(z->id);
        if (v.deveFechar && isZoneOpen(z->id)) {
            closeZoneByRadio(z->id); // helper existente de fechamento
            auditGateway(AuditOrigin::INTERTRAVAMENTO, AuditAction::FECHAR, z->id, AuditResult::OK, z->node);
            openGate.release(z->id);
        }
    }
```

(`entryAt`/`isZoneOpen`/`closeZoneByRadio`/`auditGateway` são helpers do gateway — usar os nomes reais ao implementar; adicionar `entryAt(size_t)` ao `StationTelemetryCache` se não existir.)

- [ ] **Step 3:** Compilar (`-f test_open_gate`). **Commit** `feat(irrigation): gateway avalia intertravamentos e fecha_e_bloqueia (fase 6b)`.

## Task 13: Gate de admissão no caminho de abertura do cronograma

**Files:** Modify `IrrigationGateway.cpp` (onde `ProgramScheduler::tick` produz `SchedAction::OPEN`).

- [ ] **Step 1:** Onde hoje uma `SchedAction::OPEN` vira comando de abrir zona, interpor o gate:

```cpp
    if (act.type == SchedAction::Type::OPEN) {
        ZoneVerdict v = interlockEngine.zoneVerdict(act.zoneId);
        if (v.bloqueada) {
            auditGateway(AuditOrigin::INTERTRAVAMENTO, AuditAction::CMD_REJEITADO, act.zoneId, AuditResult::NACK, 0);
            // não abre; cronograma tenta de novo no próximo tick (regra ainda ativa = segue bloqueado)
        } else if (openGate.request(act.zoneId, act.durationS) == OpenGate::Decision::ADMIT) {
            openZoneByRadio(act.zoneId, act.durationS);
        } // HOLD: enfileirado com a durationS, abre quando liberar (abaixo)
    } else if (act.type == SchedAction::Type::CLOSE) {
        openGate.release(act.zoneId);
        closeZoneByRadio(act.zoneId);
    }
```

- [ ] **Step 2:** Após processar o tick, drenar a fila enquanto houver capacidade. `nextAdmittable()` já devolve `{zoneId, durationS}` (a duração do passo que enfileirou — Task 6):

```cpp
    OpenGate::Pending p;
    while ((p = openGate.nextAdmittable()).zoneId != 0) {
        if (interlockEngine.zoneVerdict(p.zoneId).bloqueada) continue; // desiste se bloqueou nesse meio-tempo
        openGate.request(p.zoneId, p.durationS); // registra a abertura
        openZoneByRadio(p.zoneId, p.durationS);
    }
```

- [ ] **Step 3:** Compilar. **Commit** `feat(irrigation): fila de simultaneidade no cronograma (fase 6b)`.

## Task 14: Push das regras locais (v5) por estação

**Files:** Modify `IrrigationGateway.cpp` (montagem do blob desejado por estação, onde `StationEntry.blob` é preenchido).

- [ ] **Step 1:** Ao (re)montar o config desejado de uma estação, preencher `localInterlocks[]` com o subconjunto de regras globais `IL_SENSOR` cujo `node` == a estação e cuja ação referencia saídas locais. Mapear a zona da regra → índice de saída local via `ZoneTable` (`zone.index`, `zone.tipo`):

```cpp
    IrrigationSettings desired; migrateIrrigationSettings(entry.blob, sizeof(entry.blob), desired);
    uint8_t li = 0;
    for (size_t i = 0; i < interlocks.count() && li < IrrigationSettings::MAX_LOCAL_INTERLOCKS; i++) {
        const InterlockRule *r = interlocks.ruleAt(i);
        if (r->tipo != IL_SENSOR || r->node != entry.node) continue;
        uint8_t mask = 0;
        for (uint8_t zi = 0; zi < 8; zi++) {
            uint8_t zid = r->todas ? 0 : r->zoneIds[zi];
            // p/ regras "todas", incluir todas as zonas locais desta estação:
            // (varredura de ZoneTable por node == entry.node)
            const Zone *z = r->todas ? zones.zoneAt(zi) : zones.byId(zid);
            if (!z) { if (r->todas) continue; else break; }
            if (z->node == entry.node) mask |= (uint8_t)(1u << z->index);
            if (!r->todas && zid == 0) break;
        }
        if (!mask) continue;
        desired.localInterlocks[li++] = { r->sensorIdx, r->condicao, r->acao, mask,
                                          r->valorCenti, r->histereseCenti, 0 };
    }
    desired.version = 5;
    // recomputar epoch e blob desejado (o mecanismo de push §5.4 já existente cuida do envio)
```

(Simplificar a varredura "todas" conforme os helpers reais da `ZoneTable`; a intenção: `saidasMask` = bits das saídas locais da estação cobertas pela regra.)

- [ ] **Step 2:** Compilar. **Commit** `feat(irrigation): push das regras locais v5 por estação (fase 6b)`.

## Task 15: `MSG_CMD_MAINT` — janela de manutenção do tamper

**Files:** Modify `IrrigationProtocol.h`/`.cpp` (protocolo), `IrrigationGateway.cpp` (envio), `IrrigationModule.cpp` (estação recebe). Test: `test/test_irrigation_protocol/test_main.cpp`.

- [ ] **Step 1: Teste que falha** — em `test_irrigation_protocol`:

```cpp
static void test_cmd_maint_roundtrip()
{
    using namespace IrrigationProto;
    uint8_t buf[64];
    CmdMaint m{15}; // 15 min
    size_t n = encodeCmdMaint(buf, sizeof(buf), 42, m);
    TEST_ASSERT_GREATER_THAN(0, n);
    Header h; TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_CMD_MAINT, h.type);
    CmdMaint out; TEST_ASSERT_TRUE(decodeCmdMaint(buf, n, out));
    TEST_ASSERT_EQUAL_UINT16(15, out.durationMin);
}
```

- [ ] **Step 2: Verificar RED** — `... -f test_irrigation_protocol`.

- [ ] **Step 3: Implementar protocolo** — em `IrrigationProtocol.h`: adicionar ao enum `MsgType` (append-only) `MSG_CMD_MAINT = 13,`; struct + protótipos:

```cpp
struct CmdMaint { uint16_t durationMin; }; // 0 = fechar janela agora
size_t encodeCmdMaint(uint8_t *buf, size_t len, uint32_t seq, const CmdMaint &m);
bool decodeCmdMaint(const uint8_t *buf, size_t len, CmdMaint &out);
```

Em `IrrigationProtocol.cpp`, implementar espelhando `encodeCmdGpo`/`decodeCmdGpo` (header + `durationMin` LE). **Não** mudar `VERSION`.

- [ ] **Step 4: Verificar GREEN** (protocolo). Depois a cola:
  - Gateway: helper `sendMaintWindow(node, minutes)` que encoda e envia (chamado pelo endpoint da Task 18).
  - Estação (`IrrigationModule`): no dispatch por `header.type`, tratar `MSG_CMD_MAINT` → abrir janela de manutenção do tamper por `durationMin` (reusar o mecanismo de janela da 6a que já suprime o `EV_TAMPER` enquanto o portal está aberto; agora também por comando), responder ACK. Auditar `AuditOrigin::PAINEL, AuditAction::TAMPER? ` — usar uma ação existente; se nenhuma servir, apenas registrar via mecanismo de evento local (sem novo enum).

- [ ] **Step 5:** Compilar firmware (`-f test_irrigation_protocol`). **Commit** `feat(irrigation): CMD_MAINT — janela de manutenção do tamper (fase 6b)`.

## Task 16: Persistência do log em flash (glue LittleFS)

**Files:** Modify `IrrigationGateway.h`/`.cpp`. Create adaptador `LittleFsByteStore` (impl de `IByteStore` sobre um arquivo de tamanho fixo).

- [ ] **Step 1:** Implementar `IByteStore` sobre LittleFS lendo/escrevendo em `/prefs/irrigation_audit.dat` de tamanho fixo `HEADER + 16384*16 = 262160` bytes (~256KB). Pré-alocar o arquivo no `begin` (escrever zeros se ausente/curto). Padrão de FS/rename da 6a (`/prefs/*.tmp` + rename como allowlist) aplica-se ao header; os slots são escritos in-place (append por offset, sem staging).

- [ ] **Step 2:** Instanciar `FlashAuditRing` no gateway; substituir/duplicar o `auditGateway(...)` helper para também `ring.append(rec)`. Flush do header é feito pelo próprio `append` (debounce opcional: acumular N writes antes de reescrever o header, aceitando perder o índice dos últimos N num crash — **manter simples: header a cada append** já que 256KB/16B suporta e a escrita é rara).

- [ ] **Step 3:** Compilar. **Commit** `feat(irrigation): log de auditoria persistente em flash no gateway (fase 6b)`.

---

# MARCO 4 — Frontend

## Task 17: Builders/parsers do painel (host-testado)

**Files:** Modify `IrrigationWebApi.h`/`.cpp`. Test: `test/test_web_api/test_main.cpp`.

- [ ] **Step 1: Testes que falham** — adicionar casos p/ cada novo builder/parser:
  - `buildInterlocks(const InterlockTable&, buf, cap)` → JSON array de regras.
  - `parseInterlockUpsert(json, len, InterlockRule&)` → preenche regra (id, tipo, node, sensor, condicao, valor, histerese, acao, zonas[], todas, mensagem, maxAbertas).
  - `parseInterlockDelete(json, len, uint8_t&)`.
  - `buildSensorsGateway(const StationView*, n, const SensorNameTable&, buf, cap)` → sensores por estação c/ nome + valor.
  - `parseMaintWindow(json, len, uint32_t& node, uint16_t& minutes)`.
  - `parseSensorName(json, len, uint32_t& node, uint8_t& idx, char name[16])`.
  - `buildAuditPage(const FlashAuditRing&, size_t maxRecords, buf, cap)` (ou reusar `toJson` diretamente no endpoint).

  Exemplo (um caso, espelhar os outros do estilo `parseZoneUpsert`):

```cpp
static void test_parse_interlock_upsert()
{
    using namespace IrrigationWeb;
    const char *j = "{\"id\":2,\"tipo\":0,\"node\":2712847316,\"sensor\":1,"
                    "\"condicao\":2,\"valor\":150,\"histerese\":10,\"acao\":1,"
                    "\"zonas\":[2,5],\"todas\":false,\"mensagem\":\"pressao baixa\"}";
    InterlockRule r;
    ParseResult pr = parseInterlockUpsert(j, strlen(j), r);
    TEST_ASSERT_TRUE(pr.ok);
    TEST_ASSERT_EQUAL_UINT8(2, r.id);
    TEST_ASSERT_EQUAL_UINT32(2712847316u, r.node);
    TEST_ASSERT_EQUAL_UINT8(5, r.zoneIds[1]);
    TEST_ASSERT_EQUAL_STRING("pressao baixa", r.mensagem);
}
```

> `JsonReader` da 5a só lê nível superior escalar; para o array `zonas` seguir o approach usado em `parseProgramUpsert` (que já parseia array de steps — ler esse código e replicar o scanner de array para inteiros).

- [ ] **Step 2: Verificar RED** — `... -f test_web_api`.
- [ ] **Step 3: Implementar** os builders/parsers em `IrrigationWebApi.cpp` (usar `JsonWriter`/`JsonReader`; para `zonas`, replicar o parser de array de `parseProgramUpsert`).
- [ ] **Step 4: Verificar GREEN**. **Commit** `feat(irrigation): web builders/parsers de intertravamentos/sensores/log (fase 6b)`.

## Task 18: Endpoints (CI-only)

**Files:** Modify `IrrigationWebEndpoints.cpp` (sob `#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER`).

- [ ] **Step 1:** Adicionar rotas seguindo o padrão das rotas 5a existentes:
  - `GET /api/interlocks` → `buildInterlocks`.
  - `POST /api/interlocks` → `parseInterlockUpsert` → `interlocks.upsert` → salvar.
  - `POST /api/interlocks/delete` → `parseInterlockDelete` → `removeById` → salvar.
  - `GET /api/sensors` → `buildSensorsGateway`.
  - `POST /api/sensors/name` → `parseSensorName` → `sensorNames.set` → salvar.
  - `GET /api/audit?fmt=json|csv&n=N` → `ring.toJson`/`ring.toCsv` (buffer heap ~16KB; liberar após enviar).
  - `POST /api/maint` → `parseMaintWindow` → `sendMaintWindow(node, minutes)`.

> `/api/audit` NÃO carrega o log inteiro (256KB) — usa `maxRecords` (ex.: 500) e o buffer heap. Documentar como a 6a fez com `/api/portal/log`.

- [ ] **Step 2:** Compilar (`-f test_web_api` compila o firmware). **Commit** `feat(irrigation): endpoints de intertravamentos/sensores/log/maint (fase 6b)` — CI-gated, revisar à mão.

## Task 19: UI do painel

**Files:** Modify `data/irrigacao/index.html`, `data/irrigacao/app.js`, `data/irrigacao/style.css`.

- [ ] **Step 1:** Adicionar seções ao painel do gateway (reusar `.tabs`/`.tab` e classes existentes; **não** regredir o portal de campo escopado em `.field-portal`):
  - **Sensores**: por estação, nome editável (`POST /api/sensors/name`) + valor ao vivo (`GET /api/sensors`, polling como o overview).
  - **GPO**: controles ligar/desligar (reusa `parseCommand`/comando existente; GPO biestável exige `confirm()`).
  - **Intertravamentos**: lista + form de CRUD (`GET/POST /api/interlocks`, delete). Campos conforme `InterlockRule`.
  - **Log**: tabela paginada (`GET /api/audit?fmt=json&n=500`) com filtros client-side por origem/ação/estação + botões "Exportar CSV/JSON" (link direto p/ `/api/audit?fmt=csv`).
  - **Tamper**: botão "Abrir janela de manutenção" por estação (`POST /api/maint` com minutos).
  - Rótulos de origem/ação: replicar os arrays `ORIGENS`/`ACOES`/`RESULTADOS` do `data/irrigacao/portal/app.js` (contrato de 3 vias — manter idênticos).

- [ ] **Step 2:** Sem teste nativo (estático). Verificar JSON dos builders bate com o que o JS espera. **Commit** `feat(irrigation): UI do painel — sensores/GPO/intertravamentos/log/tamper (fase 6b)`.

## Task 20: Roadmap + memória

**Files:** Modify `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md`.

- [ ] **Step 1:** Na linha da Fase 6, marcar 6b concluída (data 2026-07-22, plano deste arquivo). Ajustar o texto "6b (gateway) futuro" → concluída, resumindo o entregue.
- [ ] **Step 2: Commit** `docs(irrigation): roadmap fase 6 (6b) concluída`.
- [ ] **Step 3:** Rodar a suíte inteira uma vez e confirmar 54/54 (ou 55/55 se a Task 10 criou suíte nova):

```powershell
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh
```

- [ ] **Step 4:** Atualizar memória `irrigation-phase6a-status` → nova entrada `irrigation-phase6b-status` (o executor/assistente cuida disto ao fim).

---

## Notas finais p/ o executor

- **Ordem dos marcos importa**: A (puro, testável) antes de B (cola). Não comece a cola sem as suítes A verdes.
- **CI-gated (nunca compila local)**: `IrrigationWebEndpoints.cpp`, endpoints do portal. Revisar à mão contra `esp32_https_server`. Conferir job ESP32 (tbeam/gateway) + `trunk fmt` após push.
- **Contrato de 3 vias** (enums de auditoria): `AuditLog.h` ↔ call sites de `auditGateway` ↔ `ORIGENS`/`ACOES`/`RESULTADOS` no JS. Só APPEND. `INTERTRAVAMENTO` já existe.
- **Sem bump de `VERSION`**: MAINT é novo `MsgType` retrocompatível; Settings v5 é ABI de blob, não de protocolo.
- **ABI**: `StationEntry.blob` continua 128 B no `GatewayTables.h`, mas o blob v5 tem 176 B → **aumentar `StationEntry::blob` p/ 176** e tratar a rejeição por tamanho do registro antigo (como a 6a fez de 52→128). Isto é parte da Task 14; adicionar o ajuste do `blob[176]` + `static_assert` de `STATION_ENTRY` lá.
