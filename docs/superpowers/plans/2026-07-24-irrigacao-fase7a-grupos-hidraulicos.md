# Fase 7a — Grupos hidráulicos (motor) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Orquestração de grupos bomba/válvula no gateway — máquina de estados guiada por ACK que sequencia abertura/fechamento de válvulas com a bomba, respeitando o invariante "bomba ligada ⇒ ≥ min_abertas válvulas confirmadas".

**Architecture:** `HydraulicGroupTable` (config + persistência em arquivo LittleFS próprio) + `HydraulicGroupEngine` (unidade pura, guiada por ACK/tempo injetados, testável nativo). O engine fica no caminho do comando entre `ProgramScheduler` (fonte de tempo) e o rádio: zonas agrupadas viram `setDesired`; zonas livres seguem o caminho atual. Sem mudança na ABI de estação (bomba = zona `gpo`).

**Tech Stack:** C++17 Arduino-free (portável p/ ESP32 e native), Unity test framework, PlatformIO `coverage` env via Docker (`Dockerfile.test`), persistência via staged-write LittleFS.

**Spec:** `docs/superpowers/specs/2026-07-24-irrigacao-fase7a-grupos-hidraulicos-design.md`

## Referências do codebase (ler antes de começar)

- `src/modules/irrigation/GatewayTables.h` — padrão de tabela (`ZoneTable`: `upsert`/`byId`/`serialize`/`deserialize`, MAGIC).
- `src/modules/irrigation/OpenGate.h/.cpp` + `test/test_open_gate/test_main.cpp` — estilo de unidade pura + teste.
- `src/modules/irrigation/CommandTracker.h` + `test/test_irrigation_cmdtracker/test_main.cpp` — idioma de injeção determinística de tempo (`nowMs`) e seq/ACK.
- `src/modules/irrigation/InterlockTable.cpp` — serialização MAGIC+ver+count+CRC de tabela.
- `src/modules/irrigation/IrrigationModule.cpp:1334-1435` — `loadGatewayState`/`saveGatewayState`/`loadInterlocks`/`saveInterlocks` (padrão staged-write a espelhar).
- `src/modules/irrigation/IrrigationModule.cpp:1557-1582` — `gwSendValveCmd` (expor seq).
- `src/modules/irrigation/IrrigationModule.cpp:1896-2060` — `gwTick` (ponto de integração).
- `src/modules/irrigation/IrrigationModule.cpp:2210-2240` — `handleGwAck` (hook `onAck`).
- `src/modules/irrigation/AuditLog.h` — enum `AuditOrigin` (append-only).

## Como rodar os testes

Esta máquina (Windows) **não** roda o suite nativo direto — usar o container:

```powershell
docker build -f Dockerfile.test -t mesh-test .   # 1x
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f <suite>
```

Exit: 0 GREEN · 1 RED · 2 AMBER · 3 FILTERED. Um `-f` compila o firmware inteiro (valida edições de módulo), rende FILTERED (3), ~4 min.

**Inner-loop rápido** (só p/ código Arduino-free — tabela e engine): compila os arquivos isolados com g++ e um `main` scratch, ou confie no `run-tests.sh -f`. Spot-check de sintaxe:
```
C:\Users\jmelo\w64devkit\bin\g++.exe -std=c++17 -Isrc -fsyntax-only src/modules/irrigation/HydraulicGroupTable.cpp
```

Ao final, um `./bin/run-tests.sh` completo (sem `-f`) deve render GREEN 56/56.

---

## Task 1: HydraulicGroupTable — struct + tabela + serialização

**Files:**
- Create: `src/modules/irrigation/HydraulicGroupTable.h`
- Create: `src/modules/irrigation/HydraulicGroupTable.cpp`
- Test: `test/test_hydraulic_group_table/test_main.cpp`

- [ ] **Step 1: Criar o header**

`src/modules/irrigation/HydraulicGroupTable.h`:
```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

// §8.13 — grupo hidráulico (bomba + válvulas). Orquestração 100% gateway;
// a bomba é uma zona gpo comum (bombaZoneId), sem ABI nova de estação.
struct HydraulicGroup {
    uint8_t  id = 0;               // 0 = slot vazio
    char     name[16] = {0};
    uint8_t  bombaZoneId = 0;      // zona gpo da bomba; 0 = grupo SEM bomba (== §8.10 simultaneidade)
    uint8_t  zoneIds[8] = {0};     // válvulas membro
    uint8_t  zoneCount = 0;
    uint8_t  minOpen = 1;          // min_abertas_com_bomba
    uint8_t  maxOpen = 1;          // max_abertas (0 = sem teto)
    uint8_t  transicao = 0;        // 0 = abrir_antes_de_fechar, 1 = fechar_antes_de_abrir
    uint16_t overlapS = 10;        // sobreposicao_s
    uint16_t startAfterOpenS = 5;  // partida_apos_abrir_s
    uint16_t stopBeforeCloseS = 8; // parar_antes_de_fechar_s
    uint16_t minRunMin = 5;        // funcionamento_min_min
    uint8_t  maxStartsHour = 6;    // max_partidas_hora
};

class HydraulicGroupTable {
  public:
    static constexpr size_t MAX = 8;
    static constexpr uint32_t MAGIC = 0x49484731; // "IHG1"

    bool upsert(const HydraulicGroup &g);           // valida; false = cheia/inválida
    bool removeById(uint8_t id);
    const HydraulicGroup *byId(uint8_t id) const;       // nullptr = ausente
    const HydraulicGroup *byZone(uint8_t zoneId) const; // grupo que contém a zona-membro
    const HydraulicGroup *byPumpZone(uint8_t zoneId) const; // grupo cuja bomba é essa zona
    const HydraulicGroup *groupAt(size_t index) const;  // index-ésimo grupo ocupado
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);     // falha => tabela vazia

  private:
    HydraulicGroup groups[MAX];
    bool zoneUsedByOther(uint8_t zoneId, uint8_t exceptId) const;
    bool valid(const HydraulicGroup &g) const;
};
```

- [ ] **Step 2: Escrever o teste que falha**

`test/test_hydraulic_group_table/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/HydraulicGroupTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static HydraulicGroup mk(uint8_t id, uint8_t pump, std::initializer_list<uint8_t> zs)
{
    HydraulicGroup g;
    g.id = id;
    g.bombaZoneId = pump;
    g.zoneCount = 0;
    for (uint8_t z : zs)
        g.zoneIds[g.zoneCount++] = z;
    g.minOpen = 1;
    g.maxOpen = 2;
    return g;
}

static void test_upsert_e_lookup()
{
    HydraulicGroupTable t;
    TEST_ASSERT_TRUE(t.upsert(mk(1, 9, {1, 2, 3})));
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
    TEST_ASSERT_NOT_NULL(t.byId(1));
    TEST_ASSERT_EQUAL_UINT8(9, t.byId(1)->bombaZoneId);
    TEST_ASSERT_EQUAL_UINT8(1, t.byZone(2)->id);      // zona-membro -> grupo
    TEST_ASSERT_EQUAL_UINT8(1, t.byPumpZone(9)->id);  // bomba -> grupo
    TEST_ASSERT_NULL(t.byZone(9));                     // bomba não é membro
    TEST_ASSERT_NULL(t.byId(2));
}

static void test_zona_em_dois_grupos_rejeitada()
{
    HydraulicGroupTable t;
    TEST_ASSERT_TRUE(t.upsert(mk(1, 9, {1, 2})));
    TEST_ASSERT_FALSE(t.upsert(mk(2, 8, {2, 3}))); // zona 2 já usada
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
}

static void test_validacoes()
{
    HydraulicGroupTable t;
    HydraulicGroup g = mk(1, 9, {1, 2});
    g.minOpen = 3; // > zoneCount(2)
    TEST_ASSERT_FALSE(t.upsert(g));
    g.minOpen = 1;
    g.maxOpen = 0; // sem teto ok
    TEST_ASSERT_TRUE(t.upsert(g));
    HydraulicGroup h = mk(2, 5, {5, 6}); // bomba 5 é membro do próprio grupo
    TEST_ASSERT_FALSE(t.upsert(h));
    HydraulicGroup z = mk(0, 9, {7}); // id 0 inválido
    TEST_ASSERT_FALSE(t.upsert(z));
}

static void test_serialize_roundtrip()
{
    HydraulicGroupTable a;
    a.upsert(mk(1, 9, {1, 2, 3}));
    a.upsert(mk(2, 0, {4, 5})); // grupo sem bomba
    uint8_t buf[600];
    size_t n = a.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    HydraulicGroupTable b;
    TEST_ASSERT_TRUE(b.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT32(2, b.count());
    TEST_ASSERT_EQUAL_UINT8(9, b.byId(1)->bombaZoneId);
    TEST_ASSERT_EQUAL_UINT8(0, b.byId(2)->bombaZoneId);
    TEST_ASSERT_EQUAL_UINT8(3, b.byId(1)->zoneCount);
}

static void test_deserialize_corrompido_zera()
{
    HydraulicGroupTable a;
    a.upsert(mk(1, 9, {1}));
    uint8_t buf[600];
    size_t n = a.serialize(buf, sizeof(buf));
    buf[0] ^= 0xFF; // corrompe MAGIC
    HydraulicGroupTable b;
    b.upsert(mk(7, 3, {7}));
    TEST_ASSERT_FALSE(b.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT32(0, b.count()); // falha => vazia
}

static void test_remove()
{
    HydraulicGroupTable t;
    t.upsert(mk(1, 9, {1, 2}));
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_FALSE(t.removeById(1));
    TEST_ASSERT_EQUAL_UINT32(0, t.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_upsert_e_lookup);
    RUN_TEST(test_zona_em_dois_grupos_rejeitada);
    RUN_TEST(test_validacoes);
    RUN_TEST(test_serialize_roundtrip);
    RUN_TEST(test_deserialize_corrompido_zera);
    RUN_TEST(test_remove);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Rodar o teste — deve falhar no link (sem .cpp)**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_hydraulic_group_table`
Expected: RED (undefined reference a `HydraulicGroupTable::upsert` etc.)

- [ ] **Step 4: Implementar `HydraulicGroupTable.cpp`**

`src/modules/irrigation/HydraulicGroupTable.cpp`:
```cpp
#include "HydraulicGroupTable.h"
#include <string.h>

namespace
{
// Entrada fixa: id(1)+name(16)+bomba(1)+zoneIds(8)+zoneCount(1)+minOpen(1)+maxOpen(1)
//   +transicao(1)+overlapS(2)+startAfterOpenS(2)+stopBeforeCloseS(2)+minRunMin(2)+maxStartsHour(1) = 41 B
constexpr size_t ENTRY = 1 + 16 + 1 + 8 + 1 + 1 + 1 + 1 + 2 + 2 + 2 + 2 + 1;

uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}
} // namespace

bool HydraulicGroupTable::zoneUsedByOther(uint8_t zoneId, uint8_t exceptId) const
{
    for (const auto &g : groups) {
        if (g.id == 0 || g.id == exceptId)
            continue;
        for (uint8_t i = 0; i < g.zoneCount; i++)
            if (g.zoneIds[i] == zoneId)
                return true;
    }
    return false;
}

bool HydraulicGroupTable::valid(const HydraulicGroup &g) const
{
    if (g.id == 0 || g.zoneCount == 0 || g.zoneCount > 8)
        return false;
    if (g.minOpen == 0 || g.minOpen > g.zoneCount)
        return false;
    if (g.maxOpen != 0 && g.maxOpen < g.minOpen)
        return false;
    for (uint8_t i = 0; i < g.zoneCount; i++) {
        uint8_t z = g.zoneIds[i];
        if (z == 0 || z == g.bombaZoneId)   // membro nulo ou == bomba
            return false;
        for (uint8_t j = i + 1; j < g.zoneCount; j++)
            if (g.zoneIds[j] == z)           // duplicata interna
                return false;
        if (zoneUsedByOther(z, g.id))
            return false;
    }
    return true;
}

bool HydraulicGroupTable::upsert(const HydraulicGroup &g)
{
    if (!valid(g))
        return false;
    for (auto &s : groups)
        if (s.id == g.id) {
            s = g;
            return true;
        }
    for (auto &s : groups)
        if (s.id == 0) {
            s = g;
            return true;
        }
    return false; // cheia
}

bool HydraulicGroupTable::removeById(uint8_t id)
{
    if (id == 0)
        return false;
    for (auto &s : groups)
        if (s.id == id) {
            s = HydraulicGroup{};
            return true;
        }
    return false;
}

const HydraulicGroup *HydraulicGroupTable::byId(uint8_t id) const
{
    if (id == 0)
        return nullptr;
    for (const auto &s : groups)
        if (s.id == id)
            return &s;
    return nullptr;
}

const HydraulicGroup *HydraulicGroupTable::byZone(uint8_t zoneId) const
{
    if (zoneId == 0)
        return nullptr;
    for (const auto &s : groups) {
        if (s.id == 0)
            continue;
        for (uint8_t i = 0; i < s.zoneCount; i++)
            if (s.zoneIds[i] == zoneId)
                return &s;
    }
    return nullptr;
}

const HydraulicGroup *HydraulicGroupTable::byPumpZone(uint8_t zoneId) const
{
    if (zoneId == 0)
        return nullptr;
    for (const auto &s : groups)
        if (s.id != 0 && s.bombaZoneId == zoneId)
            return &s;
    return nullptr;
}

const HydraulicGroup *HydraulicGroupTable::groupAt(size_t index) const
{
    size_t seen = 0;
    for (const auto &s : groups) {
        if (s.id == 0)
            continue;
        if (seen == index)
            return &s;
        seen++;
    }
    return nullptr;
}

size_t HydraulicGroupTable::count() const
{
    size_t c = 0;
    for (const auto &s : groups)
        if (s.id != 0)
            c++;
    return c;
}

size_t HydraulicGroupTable::serialize(uint8_t *buf, size_t cap) const
{
    uint8_t cnt = (uint8_t)count();
    size_t need = 6 + (size_t)cnt * ENTRY + 4; // header(6) + entries + crc(4)
    if (cap < need)
        return 0;
    memcpy(buf, &MAGIC, 4);
    buf[4] = 1; // versão
    buf[5] = cnt;
    size_t off = 6;
    for (const auto &g : groups) {
        if (g.id == 0)
            continue;
        buf[off] = g.id;
        memcpy(buf + off + 1, g.name, 16);
        buf[off + 17] = g.bombaZoneId;
        memcpy(buf + off + 18, g.zoneIds, 8);
        buf[off + 26] = g.zoneCount;
        buf[off + 27] = g.minOpen;
        buf[off + 28] = g.maxOpen;
        buf[off + 29] = g.transicao;
        memcpy(buf + off + 30, &g.overlapS, 2);
        memcpy(buf + off + 32, &g.startAfterOpenS, 2);
        memcpy(buf + off + 34, &g.stopBeforeCloseS, 2);
        memcpy(buf + off + 36, &g.minRunMin, 2);
        buf[off + 38] = g.maxStartsHour;
        off += ENTRY;
    }
    uint32_t c = crc32(buf, off);
    memcpy(buf + off, &c, 4);
    return off + 4;
}

bool HydraulicGroupTable::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : groups)
        s = HydraulicGroup{};
    if (n < 10)
        return false;
    uint32_t m;
    memcpy(&m, buf, 4);
    if (m != MAGIC || buf[4] != 1)
        return false;
    uint8_t cnt = buf[5];
    if (cnt > MAX)
        return false;
    size_t need = 6 + (size_t)cnt * ENTRY + 4;
    if (n != need)
        return false;
    uint32_t stored;
    memcpy(&stored, buf + need - 4, 4);
    if (crc32(buf, need - 4) != stored)
        return false;
    size_t off = 6;
    for (uint8_t i = 0; i < cnt; i++, off += ENTRY) {
        HydraulicGroup &g = groups[i];
        g.id = buf[off];
        memcpy(g.name, buf + off + 1, 16);
        g.name[15] = 0;
        g.bombaZoneId = buf[off + 17];
        memcpy(g.zoneIds, buf + off + 18, 8);
        g.zoneCount = buf[off + 26];
        g.minOpen = buf[off + 27];
        g.maxOpen = buf[off + 28];
        g.transicao = buf[off + 29];
        memcpy(&g.overlapS, buf + off + 30, 2);
        memcpy(&g.startAfterOpenS, buf + off + 32, 2);
        memcpy(&g.stopBeforeCloseS, buf + off + 34, 2);
        memcpy(&g.minRunMin, buf + off + 36, 2);
        g.maxStartsHour = buf[off + 38];
        if (g.zoneCount > 8) { // guarda contra blob adulterado
            for (auto &s : groups)
                s = HydraulicGroup{};
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 5: Rodar o teste — deve passar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_hydraulic_group_table`
Expected: FILTERED (exit 3) — o suite rodou verde, os demais foram filtrados. Confirmar a linha `test_hydraulic_group_table [PASSED]`.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/HydraulicGroupTable.h src/modules/irrigation/HydraulicGroupTable.cpp test/test_hydraulic_group_table/test_main.cpp
git commit -m "feat(irrigation): HydraulicGroupTable — config + serializacao (fase 7a)"
```

---

## Task 2: HydraulicGroupEngine — header + happy path (min=1, abrir_antes_de_fechar)

**Files:**
- Create: `src/modules/irrigation/HydraulicGroupEngine.h`
- Create: `src/modules/irrigation/HydraulicGroupEngine.cpp`
- Test: `test/test_hydraulic_group_engine/test_main.cpp`

Convenções do engine (fixam todos os testes seguintes):
- `tick()` é o **único emissor**: coleta ≤`cap` `GroupEmit` prontos p/ envio, conforme estado + timers + conjunto confirmado.
- `onAck()`/`onCmdFailed()`/`observeActual()` só atualizam estado; o próximo `tick()` emite.
- ACK correlacionado por **seq próprio** (`noteSent` registra `seq→(zona,ação)`); não depende do `CommandTracker`.
- Duração da bomba embarcada = `min(durZonaCorrente + PUMP_MARGIN_S, PUMP_CEILING_S)`, renovada a cada transição.

- [ ] **Step 1: Criar o header completo**

`src/modules/irrigation/HydraulicGroupEngine.h`:
```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "modules/irrigation/GatewayTables.h"        // ZoneTable
#include "modules/irrigation/HydraulicGroupTable.h"

struct GroupEmit {
    uint32_t node = 0;
    uint8_t  index = 0;     // índice da saída na estação
    uint8_t  tipo = 0;      // 0 valvula, 1 gpo
    uint8_t  zoneId = 0;
    uint8_t  action = 0;    // 1 abrir, 0 fechar
    uint16_t durationS = 0;
};

class HydraulicGroupEngine {
  public:
    static constexpr size_t MAX_GROUPS = HydraulicGroupTable::MAX; // 8
    static constexpr size_t MAX_ZONES = 8;
    static constexpr uint16_t PUMP_MARGIN_S = 120;
    static constexpr uint16_t PUMP_CEILING_S = 7200; // estação clampa em 120 min
    static constexpr uint32_t START_WINDOW_MS = 3600000u; // janela de max_partidas_hora

    enum class State : uint8_t {
        IDLE, OPENING, START_WAIT, PUMP_WAIT_ACK, RUNNING,
        X_OPEN_WAIT, X_OVERLAP, X_CLOSE_WAIT,
        PUMP_OFF_WAIT, DRAIN, CLOSE_LAST_WAIT, DEFERRED
    };

    enum AlertCode : uint8_t {
        GA_NONE = 0, GA_PUMP_ON, GA_PUMP_OFF,
        GA_OPEN_FAIL_RENEW, GA_OPEN_FAIL_PUMPOFF,
        GA_CLOSE_FAIL, GA_ORDERED_SHUTDOWN,
        GA_DEFER_RATE, GA_DEFER_MINOPEN, GA_REBOOT_RECONCILE
    };
    struct GroupAlert { uint8_t groupId = 0; uint8_t code = 0; uint8_t zoneId = 0; };

    void reset();
    void setDesired(uint8_t groupId, uint8_t zoneId, bool open, uint16_t durationS);
    size_t tick(const HydraulicGroupTable &tbl, const ZoneTable &zones, uint32_t nowMs,
                GroupEmit *out, size_t cap);
    void noteSent(uint32_t node, uint8_t zoneId, uint8_t action, uint32_t seq);
    void onAck(uint32_t node, uint32_t ackedSeq);
    void onCmdFailed(uint32_t node, uint8_t zoneId, uint8_t action);
    void observeActual(uint8_t groupId, uint8_t zoneId, bool open);
    bool takeAlert(GroupAlert &out);

    // Observabilidade p/ testes.
    State stateOf(uint8_t groupId) const;
    bool pumpOn(uint8_t groupId) const;

  private:
    struct ZoneRt {
        uint8_t zoneId = 0;          // 0 = slot livre
        bool wanted = false;
        uint16_t wantDurS = 0;
        bool confirmed = false;      // aberta confirmada
        uint32_t localExpiresMs = 0; // deadline do timer local (0 = fechada)
    };
    struct PendCmd {                 // um comando emitido aguardando ACK
        bool inUse = false;
        uint32_t node = 0, seq = 0;
        uint8_t zoneId = 0, action = 0;
    };
    struct GroupRt {
        State state = State::IDLE;
        bool pump = false;
        uint8_t curZone = 0;
        uint8_t nextZone = 0;
        uint32_t waitStartMs = 0;
        uint32_t lastStartMs = 0;    // última partida da bomba (bridging)
        uint32_t startRing[8] = {0}; // timestamps de partida (max_partidas_hora)
        uint8_t  startCount = 0;     // partidas na janela corrente
        PendCmd pend;                // 1 comando pendente por grupo (sequencial)
        ZoneRt zones[MAX_ZONES];
    };

    GroupRt rt[MAX_GROUPS];
    GroupAlert alertRing[16];
    size_t aHead = 0, aTail = 0, aCount = 0;

    GroupRt &rtOf(uint8_t groupId);            // groupId 1..8 -> rt[groupId-1]
    ZoneRt *findZone(GroupRt &g, uint8_t zoneId);
    ZoneRt *ensureZone(GroupRt &g, uint8_t zoneId);
    void pushAlert(uint8_t groupId, uint8_t code, uint8_t zoneId);
    // Preenche node/index/tipo a partir da ZoneTable; retorna false se zona ausente.
    bool resolve(const ZoneTable &zones, uint8_t zoneId, GroupEmit &e) const;
    uint16_t pumpDur(uint16_t zoneDurS) const;
    size_t confirmedCount(const GroupRt &g) const;
    uint8_t pruneStarts(GroupRt &g, uint32_t nowMs) const; // remove partidas > 1h, devolve count
};
```

- [ ] **Step 2: Escrever o teste de happy path (falha)**

`test/test_hydraulic_group_engine/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/HydraulicGroupEngine.h"
#include <unity.h>

using State = HydraulicGroupEngine::State;

static const uint32_t NODE = 0xAABBCCDD;

void setUp(void) {}
void tearDown(void) {}

// Monta ZoneTable: válvulas 1,2 (tipo valvula, index 0/1) + bomba zona 9 (gpo, index 0), todas em NODE.
static void seedZones(ZoneTable &z)
{
    Zone v1; v1.id = 1; v1.node = NODE; v1.tipo = 0; v1.index = 0; v1.maxMin = 120; z.upsert(v1);
    Zone v2; v2.id = 2; v2.node = NODE; v2.tipo = 0; v2.index = 1; v2.maxMin = 120; z.upsert(v2);
    Zone pb; pb.id = 9; pb.node = NODE; pb.tipo = 1; pb.index = 0; pb.maxMin = 120; z.upsert(pb);
}

static HydraulicGroup grp()
{
    HydraulicGroup g;
    g.id = 1; g.bombaZoneId = 9;
    g.zoneIds[0] = 1; g.zoneIds[1] = 2; g.zoneCount = 2;
    g.minOpen = 1; g.maxOpen = 1; g.transicao = 0; // abrir_antes_de_fechar
    g.overlapS = 10; g.startAfterOpenS = 5; g.stopBeforeCloseS = 8;
    g.minRunMin = 0; g.maxStartsHour = 0; // sem bridging/rate p/ este teste
    return g;
}

// helper: dá 1 tick e devolve o único emit esperado (falha se != 1).
static GroupEmit tick1(HydraulicGroupEngine &e, const HydraulicGroupTable &t, const ZoneTable &z, uint32_t ms)
{
    GroupEmit out[4];
    size_t n = e.tick(t, z, ms, out, 4);
    TEST_ASSERT_EQUAL_UINT32(1, n);
    return out[0];
}

static void tickNone(HydraulicGroupEngine &e, const HydraulicGroupTable &t, const ZoneTable &z, uint32_t ms)
{
    GroupEmit out[4];
    TEST_ASSERT_EQUAL_UINT32(0, e.tick(t, z, ms, out, 4));
}

static void test_happy_path_min1()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();

    // Cronograma manda abrir V1 por 600 s.
    e.setDesired(1, 1, true, 600);

    // tick 1: abre V1.
    GroupEmit em = tick1(e, t, z, 1000);
    TEST_ASSERT_EQUAL_UINT8(1, em.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, em.action);
    TEST_ASSERT_EQUAL_UINT16(600, em.durationS);
    TEST_ASSERT_EQUAL_HEX32(NODE, em.node);
    e.noteSent(NODE, 1, 1, 501);

    // sem ACK ainda -> nada.
    tickNone(e, t, z, 1000);
    e.onAck(NODE, 501); // V1 confirmada

    // ainda dentro de startAfterOpenS (5 s) -> nada.
    tickNone(e, t, z, 2000);

    // 5 s depois: liga bomba (zona 9, gpo, index 0), dur = 600+120.
    GroupEmit pump = tick1(e, t, z, 6000);
    TEST_ASSERT_EQUAL_UINT8(9, pump.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, pump.action);
    TEST_ASSERT_EQUAL_UINT8(1, pump.tipo);
    TEST_ASSERT_EQUAL_UINT16(720, pump.durationS);
    e.noteSent(NODE, 9, 1, 502);
    e.onAck(NODE, 502);

    // RUNNING, desejo satisfeito -> nada.
    tickNone(e, t, z, 6000);
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));
    TEST_ASSERT_TRUE(e.pumpOn(1));

    // Transição: cronograma fecha V1, abre V2 (mesmo tick).
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);

    // abrir_antes_de_fechar: abre V2 primeiro.
    GroupEmit o2 = tick1(e, t, z, 100000);
    TEST_ASSERT_EQUAL_UINT8(2, o2.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, o2.action);
    e.noteSent(NODE, 2, 1, 503);
    e.onAck(NODE, 503);

    // durante a sobreposição (10 s) -> nada.
    tickNone(e, t, z, 105000);

    // após overlap: fecha V1.
    GroupEmit c1 = tick1(e, t, z, 110001);
    TEST_ASSERT_EQUAL_UINT8(1, c1.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, c1.action);
    e.noteSent(NODE, 1, 0, 504);
    e.onAck(NODE, 504);
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));

    // Fim: cronograma fecha V2 (última).
    e.setDesired(1, 2, false, 0);

    // desliga bomba primeiro.
    GroupEmit poff = tick1(e, t, z, 700000);
    TEST_ASSERT_EQUAL_UINT8(9, poff.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, poff.action);
    e.noteSent(NODE, 9, 0, 505);
    e.onAck(NODE, 505);
    TEST_ASSERT_FALSE(e.pumpOn(1));

    // drena parar_antes_de_fechar_s (8 s) -> nada.
    tickNone(e, t, z, 702000);

    // fecha última válvula.
    GroupEmit clast = tick1(e, t, z, 708001);
    TEST_ASSERT_EQUAL_UINT8(2, clast.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, clast.action);
    e.noteSent(NODE, 2, 0, 506);
    e.onAck(NODE, 506);

    tickNone(e, t, z, 708001);
    TEST_ASSERT_EQUAL(State::IDLE, e.stateOf(1));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_happy_path_min1);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Rodar — deve falhar (link)**

Run: `... ./bin/run-tests.sh -f test_hydraulic_group_engine`
Expected: RED (undefined reference aos métodos do engine).

- [ ] **Step 4: Implementar o `.cpp` — núcleo + happy path**

`src/modules/irrigation/HydraulicGroupEngine.cpp` (implementar a máquina p/ passar o happy path; slices seguintes estendem `tick`):
```cpp
#include "HydraulicGroupEngine.h"

void HydraulicGroupEngine::reset() { *this = HydraulicGroupEngine{}; }

HydraulicGroupEngine::GroupRt &HydraulicGroupEngine::rtOf(uint8_t groupId) { return rt[groupId - 1]; }

HydraulicGroupEngine::ZoneRt *HydraulicGroupEngine::findZone(GroupRt &g, uint8_t zoneId)
{
    for (auto &z : g.zones)
        if (z.zoneId == zoneId)
            return &z;
    return nullptr;
}

HydraulicGroupEngine::ZoneRt *HydraulicGroupEngine::ensureZone(GroupRt &g, uint8_t zoneId)
{
    if (ZoneRt *z = findZone(g, zoneId))
        return z;
    for (auto &z : g.zones)
        if (z.zoneId == 0) {
            z = ZoneRt{};
            z.zoneId = zoneId;
            return &z;
        }
    return nullptr;
}

void HydraulicGroupEngine::pushAlert(uint8_t groupId, uint8_t code, uint8_t zoneId)
{
    if (aCount >= 16) // ring cheio: descarta o mais antigo
        { aHead = (aHead + 1) % 16; aCount--; }
    alertRing[aTail] = {groupId, code, zoneId};
    aTail = (aTail + 1) % 16;
    aCount++;
}

bool HydraulicGroupEngine::takeAlert(GroupAlert &out)
{
    if (aCount == 0)
        return false;
    out = alertRing[aHead];
    aHead = (aHead + 1) % 16;
    aCount--;
    return true;
}

bool HydraulicGroupEngine::resolve(const ZoneTable &zones, uint8_t zoneId, GroupEmit &e) const
{
    const Zone *z = zones.byId(zoneId);
    if (!z)
        return false;
    e.node = z->node;
    e.index = z->index;
    e.tipo = z->tipo;
    e.zoneId = zoneId;
    return true;
}

uint16_t HydraulicGroupEngine::pumpDur(uint16_t zoneDurS) const
{
    uint32_t d = (uint32_t)zoneDurS + PUMP_MARGIN_S;
    return (uint16_t)(d > PUMP_CEILING_S ? PUMP_CEILING_S : d);
}

size_t HydraulicGroupEngine::confirmedCount(const GroupRt &g) const
{
    size_t c = 0;
    for (const auto &z : g.zones)
        if (z.zoneId && z.confirmed)
            c++;
    return c;
}

void HydraulicGroupEngine::setDesired(uint8_t groupId, uint8_t zoneId, bool open, uint16_t durationS)
{
    if (groupId == 0 || groupId > MAX_GROUPS)
        return;
    GroupRt &g = rtOf(groupId);
    ZoneRt *z = ensureZone(g, zoneId);
    if (!z)
        return;
    z->wanted = open;
    if (open)
        z->wantDurS = durationS;
}

void HydraulicGroupEngine::noteSent(uint32_t node, uint8_t zoneId, uint8_t action, uint32_t seq)
{
    for (auto &g : rt)
        if (g.pend.inUse && g.pend.node == node && g.pend.zoneId == zoneId && g.pend.action == action) {
            g.pend.seq = seq;
            return;
        }
}

void HydraulicGroupEngine::onAck(uint32_t node, uint32_t ackedSeq)
{
    for (size_t i = 0; i < MAX_GROUPS; i++) {
        GroupRt &g = rt[i];
        if (!g.pend.inUse || g.pend.node != node || g.pend.seq != ackedSeq)
            continue;
        uint8_t za = g.pend.zoneId, ac = g.pend.action;
        g.pend.inUse = false;
        // Marca confirmação da zona (a bomba não tem ZoneRt: findZone devolve nullptr e o
        // avanço de estado vem só do switch — PUMP_WAIT_ACK/PUMP_OFF_WAIT).
        if (ZoneRt *z = findZone(g, za)) {
            z->confirmed = (ac == 1);
            if (ac == 0) { z->wanted = false; z->localExpiresMs = 0; } // fechou: sai do confirmado
        }
        // Avança o estado que aguardava este ACK:
        switch (g.state) {
        case State::OPENING:
            g.state = State::START_WAIT;
            g.waitStartMs = 0; // tick usa nowMs no próximo ciclo
            break;
        case State::PUMP_WAIT_ACK:
            g.pump = true;
            g.state = State::RUNNING;
            break;
        case State::X_OPEN_WAIT:
            g.state = State::X_OVERLAP;
            g.waitStartMs = 0;
            break;
        case State::X_CLOSE_WAIT:
            g.state = State::RUNNING;
            g.curZone = g.nextZone;
            g.nextZone = 0;
            break;
        case State::PUMP_OFF_WAIT:
            g.pump = false;
            g.state = State::DRAIN;
            g.waitStartMs = 0;
            break;
        case State::CLOSE_LAST_WAIT:
            g.state = State::IDLE;
            g.curZone = 0;
            break;
        default:
            break;
        }
        return;
    }
}

// NB: bomba não tem ZoneRt; para o ACK da bomba, o pend.zoneId carrega bombaZoneId e
// findZone devolve nullptr — o switch acima já trata pelo estado (PUMP_WAIT_ACK/PUMP_OFF_WAIT).

void HydraulicGroupEngine::onCmdFailed(uint32_t, uint8_t, uint8_t) { /* Task 5/6 */ }
void HydraulicGroupEngine::observeActual(uint8_t, uint8_t, bool) { /* Task 7 */ }

HydraulicGroupEngine::State HydraulicGroupEngine::stateOf(uint8_t groupId) const
{
    if (groupId == 0 || groupId > MAX_GROUPS)
        return State::IDLE;
    return rt[groupId - 1].state;
}
bool HydraulicGroupEngine::pumpOn(uint8_t groupId) const
{
    if (groupId == 0 || groupId > MAX_GROUPS)
        return false;
    return rt[groupId - 1].pump;
}

uint8_t HydraulicGroupEngine::pruneStarts(GroupRt &, uint32_t) const { return 0; } // Task 6

size_t HydraulicGroupEngine::tick(const HydraulicGroupTable &tbl, const ZoneTable &zones, uint32_t nowMs,
                                  GroupEmit *out, size_t cap)
{
    size_t emitted = 0;
    for (size_t i = 0; i < MAX_GROUPS && emitted < cap; i++) {
        GroupRt &g = rt[i];
        uint8_t groupId = (uint8_t)(i + 1);
        const HydraulicGroup *cfg = tbl.byId(groupId);
        if (!cfg)
            continue;
        if (g.pend.inUse)
            continue; // aguardando ACK: 1 comando em voo por grupo

        // Determina desejo: zonas wanted.
        auto firstWantedUnconfirmed = [&]() -> ZoneRt * {
            for (auto &z : g.zones)
                if (z.zoneId && z.wanted && !z.confirmed)
                    return &z;
            return nullptr;
        };
        auto anyWanted = [&]() -> bool {
            for (auto &z : g.zones)
                if (z.zoneId && z.wanted)
                    return true;
            return false;
        };
        auto confirmedNotWanted = [&]() -> ZoneRt * {
            for (auto &z : g.zones)
                if (z.zoneId && z.confirmed && !z.wanted)
                    return &z;
            return nullptr;
        };

        GroupEmit e;
        switch (g.state) {
        case State::IDLE: {
            ZoneRt *w = firstWantedUnconfirmed();
            if (!w)
                break;
            if (!resolve(zones, w->zoneId, e))
                break;
            e.action = 1;
            e.durationS = w->wantDurS;
            g.pend = {true, e.node, 0, w->zoneId, 1};
            g.curZone = w->zoneId;
            g.state = State::OPENING;
            out[emitted++] = e;
            break;
        }
        case State::START_WAIT: {
            if (g.waitStartMs == 0)
                g.waitStartMs = nowMs;
            if (nowMs - g.waitStartMs < (uint32_t)cfg->startAfterOpenS * 1000)
                break;
            // Liga bomba (se houver).
            if (cfg->bombaZoneId == 0) { // grupo sem bomba: vai direto p/ RUNNING
                g.state = State::RUNNING;
                break;
            }
            if (!resolve(zones, cfg->bombaZoneId, e))
                break;
            e.action = 1;
            e.durationS = pumpDur(g.zones[0].wantDurS ? g.zones[0].wantDurS : 600);
            g.pend = {true, e.node, 0, cfg->bombaZoneId, 1};
            g.state = State::PUMP_WAIT_ACK;
            pushAlert(groupId, GA_PUMP_ON, cfg->bombaZoneId);
            out[emitted++] = e;
            break;
        }
        case State::RUNNING: {
            ZoneRt *w = firstWantedUnconfirmed();       // nova zona a abrir (transição)
            if (w) {
                if (!resolve(zones, w->zoneId, e))
                    break;
                e.action = 1;
                e.durationS = w->wantDurS;
                g.pend = {true, e.node, 0, w->zoneId, 1};
                g.nextZone = w->zoneId;
                g.state = State::X_OPEN_WAIT; // abrir_antes_de_fechar
                out[emitted++] = e;
                break;
            }
            if (!anyWanted()) {                          // fim: desliga bomba
                if (cfg->bombaZoneId != 0 && g.pump) {
                    if (!resolve(zones, cfg->bombaZoneId, e))
                        break;
                    e.action = 0;
                    g.pend = {true, e.node, 0, cfg->bombaZoneId, 0};
                    g.state = State::PUMP_OFF_WAIT;
                    pushAlert(groupId, GA_PUMP_OFF, cfg->bombaZoneId);
                    out[emitted++] = e;
                } else {
                    g.state = State::DRAIN;
                    g.waitStartMs = 0;
                }
                break;
            }
            break;
        }
        case State::X_OVERLAP: {
            if (g.waitStartMs == 0)
                g.waitStartMs = nowMs;
            if (nowMs - g.waitStartMs < (uint32_t)cfg->overlapS * 1000)
                break;
            ZoneRt *old = confirmedNotWanted();
            if (!old) { // nada a fechar: volta a RUNNING
                g.state = State::RUNNING;
                g.curZone = g.nextZone;
                g.nextZone = 0;
                break;
            }
            if (!resolve(zones, old->zoneId, e))
                break;
            e.action = 0;
            g.pend = {true, e.node, 0, old->zoneId, 0};
            g.state = State::X_CLOSE_WAIT;
            out[emitted++] = e;
            break;
        }
        case State::DRAIN: {
            if (g.waitStartMs == 0)
                g.waitStartMs = nowMs;
            if (nowMs - g.waitStartMs < (uint32_t)cfg->stopBeforeCloseS * 1000)
                break;
            ZoneRt *last = confirmedNotWanted();
            if (!last) {
                g.state = State::IDLE;
                g.curZone = 0;
                break;
            }
            if (!resolve(zones, last->zoneId, e))
                break;
            e.action = 0;
            g.pend = {true, e.node, 0, last->zoneId, 0};
            g.state = State::CLOSE_LAST_WAIT;
            out[emitted++] = e;
            break;
        }
        default:
            break;
        }
    }
    return emitted;
}
```

> Nota de implementação: quando `onAck` fecha uma válvula (`ac==0`), `z->confirmed` vira false — a válvula sai do conjunto confirmado, e `confirmedNotWanted()` não a reencontra. Ajuste `onAck` para, no fechamento, também limpar `wanted`/`localExpiresMs` da zona.

- [ ] **Step 5: Rodar — deve passar**

Run: `... ./bin/run-tests.sh -f test_hydraulic_group_engine`
Expected: FILTERED, `test_hydraulic_group_engine [PASSED]`.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/HydraulicGroupEngine.h src/modules/irrigation/HydraulicGroupEngine.cpp test/test_hydraulic_group_engine/test_main.cpp
git commit -m "feat(irrigation): HydraulicGroupEngine — nucleo + happy path min=1 (fase 7a)"
```

---

## Task 3: Engine — fechar_antes_de_abrir + minOpen > 1

**Files:**
- Modify: `src/modules/irrigation/HydraulicGroupEngine.cpp` (ramo `RUNNING`/`IDLE`/`X_*`)
- Modify: `test/test_hydraulic_group_engine/test_main.cpp` (2 testes novos)

- [ ] **Step 1: Testes que falham**

Adicionar a `test_main.cpp` (e registrar no `setup()`):
```cpp
static void test_fechar_antes_de_abrir()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.transicao = 1; // fechar_antes_de_abrir
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();

    e.setDesired(1, 1, true, 600);
    GroupEmit em = tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1); // abre V1
    GroupEmit pump = tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2); // bomba
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));

    // transição: fecha V1 ANTES de abrir V2 (sem overlap).
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);
    GroupEmit c1 = tick1(e, t, z, 100000);
    TEST_ASSERT_EQUAL_UINT8(1, c1.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, c1.action); // fecha primeiro
    e.noteSent(NODE, 1, 0, 3); e.onAck(NODE, 3);
    GroupEmit o2 = tick1(e, t, z, 100000);
    TEST_ASSERT_EQUAL_UINT8(2, o2.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, o2.action); // abre depois
}

static void test_min_open_2()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.minOpen = 2; g.maxOpen = 2;
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();

    e.setDesired(1, 1, true, 600);
    e.setDesired(1, 2, true, 600);
    // abre as 2 válvulas antes da bomba.
    GroupEmit a = tick1(e, t, z, 1000); e.noteSent(NODE, a.zoneId, 1, 1); e.onAck(NODE, 1);
    GroupEmit b = tick1(e, t, z, 1000); e.noteSent(NODE, b.zoneId, 1, 2); e.onAck(NODE, 2);
    TEST_ASSERT_TRUE((a.zoneId == 1 && b.zoneId == 2) || (a.zoneId == 2 && b.zoneId == 1));
    TEST_ASSERT_EQUAL_UINT8(1, a.action);
    TEST_ASSERT_EQUAL_UINT8(1, b.action);
    // só então a bomba.
    GroupEmit pump = tick1(e, t, z, 6000);
    TEST_ASSERT_EQUAL_UINT8(9, pump.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, pump.action);
}
```

- [ ] **Step 2: Rodar — falha**

Run: `... ./bin/run-tests.sh -f test_hydraulic_group_engine`
Expected: RED (min>1 abre bomba cedo; fechar_antes_de_abrir emite open antes de close).

- [ ] **Step 3: Implementar**

No `tick`, ramo `IDLE`/`OPENING`→`START_WAIT`: só transicionar a `START_WAIT` quando **`confirmedCount(g) >= cfg->minOpen`**; senão continuar abrindo `firstWantedUnconfirmed()` (permanecer em `OPENING`, emitindo os opens). Ajustar `onAck` do estado `OPENING` para: se `confirmedCount(g) < cfg->minOpen`, permanecer em `OPENING` (não ir a `START_WAIT`).

No ramo `RUNNING` (transição), respeitar `cfg->transicao`:
```cpp
// dentro de RUNNING, ao achar w (zona nova desejada):
if (cfg->transicao == 1) { // fechar_antes_de_abrir
    ZoneRt *old = confirmedNotWanted();
    if (old) { // fecha a antiga primeiro
        if (!resolve(zones, old->zoneId, e)) break;
        e.action = 0;
        g.pend = {true, e.node, 0, old->zoneId, 0};
        g.nextZone = w->zoneId;
        g.state = State::X_CLOSE_WAIT; // ao ACK: volta a RUNNING; próximo tick abre a nova
        out[emitted++] = e;
        break;
    }
    // sem antiga a fechar: cai no fluxo de abrir (abaixo)
}
// abrir_antes_de_fechar (ou nada a fechar): abre a nova primeiro (código já existente)
```
Para `fechar_antes_de_abrir`, `X_CLOSE_WAIT`→`onAck` volta a `RUNNING` **sem** setar `curZone=nextZone` ainda; no próximo `tick` `RUNNING` acha `w` (nextZone ainda unconfirmed) e, já não havendo `confirmedNotWanted`, abre a nova via fluxo `abrir` (que agora leva a `X_OPEN_WAIT`→ na ausência de overlap/old, `X_OVERLAP` sem `old` volta a RUNNING). Garantir que `X_OVERLAP` com `confirmedNotWanted()==nullptr` apenas volta a RUNNING setando `curZone=nextZone` (já implementado no Task 2).

- [ ] **Step 4: Rodar — passa**

Run: `... ./bin/run-tests.sh -f test_hydraulic_group_engine`
Expected: FILTERED, PASSED.

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "feat(irrigation): engine — fechar_antes_de_abrir + minOpen>1 (fase 7a)"
```

---

## Task 4: Engine — renovação da bomba a cada transição + rastreio de deadline local

**Files:**
- Modify: `src/modules/irrigation/HydraulicGroupEngine.cpp`
- Modify: `test/test_hydraulic_group_engine/test_main.cpp`

- [ ] **Step 1: Teste que falha**

```cpp
static void test_renova_bomba_na_transicao()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();

    e.setDesired(1, 1, true, 600);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    GroupEmit p1 = tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    TEST_ASSERT_EQUAL_UINT16(720, p1.durationS);

    // transição V1->V2: espera re-envio da bomba (renovação) após abrir V2 e fechar V1.
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 300); // nova duração menor
    GroupEmit o2 = tick1(e, t, z, 100000); e.noteSent(NODE, 2, 1, 3); e.onAck(NODE, 3);
    GroupEmit c1 = tick1(e, t, z, 110001); e.noteSent(NODE, 1, 0, 4); e.onAck(NODE, 4);
    // renovação: bomba re-enviada com dur = 300+120 = 420.
    GroupEmit pr = tick1(e, t, z, 110001);
    TEST_ASSERT_EQUAL_UINT8(9, pr.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, pr.action);
    TEST_ASSERT_EQUAL_UINT16(420, pr.durationS);
}
```

- [ ] **Step 2: Rodar — falha**

Expected: RED (sem renovação após transição).

- [ ] **Step 3: Implementar**

Após `X_CLOSE_WAIT`→`onAck` (transição completa, `RUNNING`, `curZone=nextZone`), no `tick` de `RUNNING`, antes de checar novas zonas: se `g.pump` e a bomba não foi renovada nesta transição, re-emitir a bomba com `pumpDur(durDaZonaCorrente)`. Controlar com flag `g.pumpNeedsRenew` setada em `X_CLOSE_WAIT`→`onAck` e no `X_OVERLAP`→volta-a-RUNNING. Guardar `curZone`'s `wantDurS`. Registrar deadline: ao emitir open de válvula, setar `z->localExpiresMs = nowMs + durationS*1000` (usado no Task 5). Adicionar campo `bool pumpNeedsRenew` em `GroupRt` e `uint16_t curDurS`.

Renovação (novo mini-estado ou inline em RUNNING):
```cpp
case State::RUNNING: {
    if (g.pump && g.pumpNeedsRenew && cfg->bombaZoneId) {
        if (!resolve(zones, cfg->bombaZoneId, e)) break;
        e.action = 1;
        ZoneRt *cur = findZone(g, g.curZone);
        e.durationS = pumpDur(cur ? cur->wantDurS : 600);
        g.pend = {true, e.node, 0, cfg->bombaZoneId, 1};
        g.pumpNeedsRenew = false;
        g.state = State::PUMP_WAIT_ACK; // ACK: pump=true, volta a RUNNING
        out[emitted++] = e;
        break;
    }
    // ... resto do RUNNING (transição / fim) já existente
}
```
Em `PUMP_WAIT_ACK`→`onAck`, se já estava `pump==true` (renovação), apenas voltar a RUNNING (sem duplicar `GA_PUMP_ON`). Guardar `GA_PUMP_ON` só na 1ª partida.

- [ ] **Step 4: Rodar — passa**. **Step 5: Commit**

```bash
git add -A && git commit -m "feat(irrigation): engine — renovacao da bomba + deadline local (fase 7a)"
```

---

## Task 5: Engine — matriz de falhas: abrir-próxima falha

**Files:**
- Modify: `src/modules/irrigation/HydraulicGroupEngine.cpp` (`onCmdFailed`)
- Modify: `test/test_hydraulic_group_engine/test_main.cpp`

Regra (§8.13): abrir a próxima falha (sem ACK após retries) → **não fecha a corrente**: renova a corrente (re-open, reinicia timer local) + alerta `GA_OPEN_FAIL_RENEW` + retenta a próxima. Se não dá p/ renovar antes do `localExpiresMs` da corrente → **desliga a bomba primeiro** (alerta `GA_OPEN_FAIL_PUMPOFF`), deixa a corrente fechar.

- [ ] **Step 1: Testes que falham**

```cpp
static void test_open_next_falha_renova_corrente()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();
    // liga V1 + bomba
    e.setDesired(1, 1, true, 600);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    // transição: abre V2 mas FALHA
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);
    GroupEmit o2 = tick1(e, t, z, 100000); e.noteSent(NODE, 2, 1, 3);
    e.onCmdFailed(NODE, 2, 1); // sem ACK após retries
    // corrente V1 ainda longe do deadline (aberta em t=1000, dur 600s -> expira ~601000ms; agora 100000)
    GroupEmit renov = tick1(e, t, z, 100000);
    TEST_ASSERT_EQUAL_UINT8(1, renov.zoneId); // renova a corrente V1
    TEST_ASSERT_EQUAL_UINT8(1, renov.action);
    HydraulicGroupEngine::GroupAlert al;
    bool got = false;
    while (e.takeAlert(al)) if (al.code == HydraulicGroupEngine::GA_OPEN_FAIL_RENEW) got = true;
    TEST_ASSERT_TRUE(got);
}

static void test_open_next_falha_sem_renovar_desliga_bomba()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();
    e.setDesired(1, 1, true, 30); // dur curta: 30 s
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1); // V1 expira ~31000ms
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);
    tick1(e, t, z, 25000); e.noteSent(NODE, 2, 1, 3);
    e.onCmdFailed(NODE, 2, 1); // perto do deadline de V1 (31000): não dá p/ renovar com segurança
    GroupEmit poff = tick1(e, t, z, 30000);
    TEST_ASSERT_EQUAL_UINT8(9, poff.zoneId); // desliga a bomba primeiro
    TEST_ASSERT_EQUAL_UINT8(0, poff.action);
}
```

- [ ] **Step 2: Rodar — falha** (`onCmdFailed` é no-op).

- [ ] **Step 3: Implementar `onCmdFailed`**

Constante de margem: `static constexpr uint32_t RENEW_MARGIN_MS = 10000;` no header. Em `onCmdFailed(node,zoneId,action)`: achar o grupo cujo `pend` batia (ou por `nextZone==zoneId`). Limpar `g.pend.inUse`. Se `action==1` (falhou abrir a nova):
```cpp
GroupRt &g = /* grupo dono */;
g.pend.inUse = false;
ZoneRt *cur = findZone(g, g.curZone);
uint32_t now = /* precisa nowMs — ver nota */;
// política sem nowMs em onCmdFailed: setar flag e decidir no tick.
g.openFailZone = zoneId;      // novo campo
g.openFailPending = true;
g.state = State::RUNNING;     // reavaliar no tick
```
Como `onCmdFailed` não recebe `nowMs`, a decisão renovar-vs-desligar entra no `tick` (que tem `nowMs`): no `RUNNING`, se `g.openFailPending`:
```cpp
ZoneRt *cur = findZone(g, g.curZone);
bool nearDeadline = cur && (cur->localExpiresMs != 0) &&
                    (int32_t)(cur->localExpiresMs - nowMs) < (int32_t)RENEW_MARGIN_MS;
if (nearDeadline && g.pump && cfg->bombaZoneId) {
    // desliga bomba primeiro
    resolve(zones, cfg->bombaZoneId, e); e.action = 0;
    g.pend = {true, e.node, 0, cfg->bombaZoneId, 0};
    g.state = State::PUMP_OFF_WAIT;
    g.openFailPending = false;
    pushAlert(groupId, GA_OPEN_FAIL_PUMPOFF, g.openFailZone);
    out[emitted++] = e; break;
}
// renova a corrente
resolve(zones, g.curZone, e); e.action = 1;
e.durationS = cur ? cur->wantDurS : 600;
g.pend = {true, e.node, 0, g.curZone, 1};
cur->localExpiresMs = nowMs + (uint32_t)e.durationS * 1000;
g.openFailPending = false;
g.state = State::OPENING; // ACK reconfirma a corrente; volta a tentar a próxima
pushAlert(groupId, GA_OPEN_FAIL_RENEW, g.openFailZone);
out[emitted++] = e; break;
```
Adicionar campos `bool openFailPending; uint8_t openFailZone;` em `GroupRt`.

- [ ] **Step 4: Rodar — passa**. **Step 5: Commit**

```bash
git add -A && git commit -m "feat(irrigation): engine — matriz de falhas: abrir-proxima falha (fase 7a)"
```

---

## Task 6: Engine — fechar-anterior falha + encerramento ordenado; bridging + max_partidas_hora

**Files:**
- Modify: `src/modules/irrigation/HydraulicGroupEngine.cpp`
- Modify: `test/test_hydraulic_group_engine/test_main.cpp`

- [ ] **Step 1: Testes que falham**

```cpp
static void test_close_prev_falha_alerta()
{
    ZoneTable z; seedZones(z);
    HydraulicGroupTable t; t.upsert(grp());
    HydraulicGroupEngine e; e.reset();
    e.setDesired(1, 1, true, 600);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    e.setDesired(1, 1, false, 0);
    e.setDesired(1, 2, true, 600);
    tick1(e, t, z, 100000); e.noteSent(NODE, 2, 1, 3); e.onAck(NODE, 3); // abre V2
    GroupEmit c1 = tick1(e, t, z, 110001); e.noteSent(NODE, 1, 0, 4);
    e.onCmdFailed(NODE, 1, 0); // fechar V1 falha
    HydraulicGroupEngine::GroupAlert al; bool got = false;
    while (e.takeAlert(al)) if (al.code == HydraulicGroupEngine::GA_CLOSE_FAIL) got = true;
    TEST_ASSERT_TRUE(got);
    // invariante seguro: grupo segue RUNNING (válvula a mais = pressão menor), não desliga bomba.
    TEST_ASSERT_TRUE(e.pumpOn(1));
}

static void test_bridging_sem_nova_partida()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.minRunMin = 10; g.maxStartsHour = 6;
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();
    // liga V1 + bomba (1 partida)
    e.setDesired(1, 1, true, 60);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    // V1 termina; gap curto antes de V2 -> bomba fica ligada (ponte).
    e.setDesired(1, 1, false, 0);
    // sem V2 ainda: dentro de minRunMin (10 min) a bomba NÃO desliga.
    tickNone(e, t, z, 120000); // 2 min depois
    TEST_ASSERT_TRUE(e.pumpOn(1));
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));
    // chega V2 -> retoma sem nova partida.
    e.setDesired(1, 2, true, 60);
    GroupEmit o2 = tick1(e, t, z, 130000);
    TEST_ASSERT_EQUAL_UINT8(2, o2.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, o2.action);
}

static void test_max_partidas_defer()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.minRunMin = 0; g.maxStartsHour = 1; // só 1 partida/h
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();
    // 1ª partida ok
    e.setDesired(1, 1, true, 60);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    // encerra tudo
    e.setDesired(1, 1, false, 0);
    tick1(e, t, z, 70000); e.noteSent(NODE, 9, 0, 3); e.onAck(NODE, 3); // bomba off
    tick1(e, t, z, 80000); e.noteSent(NODE, 1, 0, 4); e.onAck(NODE, 4); // fecha V1
    // 2ª demanda logo em seguida: partida bloqueada (1/h) -> DEFER, válvula fica fechada.
    e.setDesired(1, 2, true, 60);
    tickNone(e, t, z, 90000); // não abre V2 (não abre válvula sem bomba)
    TEST_ASSERT_EQUAL(State::DEFERRED, e.stateOf(1));
    HydraulicGroupEngine::GroupAlert al; bool got = false;
    while (e.takeAlert(al)) if (al.code == HydraulicGroupEngine::GA_DEFER_RATE) got = true;
    TEST_ASSERT_TRUE(got);
}
```

- [ ] **Step 2: Rodar — falha.**

- [ ] **Step 3: Implementar**

`onCmdFailed` com `action==0` (fechar falhou): `pushAlert(GA_CLOSE_FAIL)`, limpar `pend.inUse`, **não** desligar a bomba (invariante seguro), voltar a `RUNNING`. Contador de falhas persistentes de fechar (`g.closeFailStreak`); ao ultrapassar `> maxOpen` violações persistentes, disparar encerramento ordenado (`GA_ORDERED_SHUTDOWN` → `PUMP_OFF_WAIT`→`DRAIN`→fecha tudo). (Teste do streak opcional; mínimo é o alerta + não-desliga.)

Bridging + rate:
- `pruneStarts(g, nowMs)`: remove de `startRing` timestamps com `nowMs - ts > START_WINDOW_MS`, devolve o count restante.
- Ao **ligar** a bomba (transição OFF→ON em `START_WAIT`/renovação-que-era-off): antes de emitir, `pruneStarts`; se `cfg->maxStartsHour != 0 && count >= maxStartsHour` **e** a bomba está desligada → `g.state = DEFERRED; pushAlert(GA_DEFER_RATE)`. Não abrir válvulas alimentadas por bomba enquanto DEFERRED.
- Registrar partida: ao confirmar a bomba ON pela 1ª vez (não renovação), `startRing[startHead++%8] = nowMs; lastStartMs = nowMs`.
- Em `RUNNING` com `!anyWanted()`: só ir a `PUMP_OFF_WAIT` se `cfg->minRunMin == 0 || (nowMs - lastStartMs) >= minRunMin*60000`. Senão **permanecer RUNNING com a bomba ligada** (ponte). Quando novo `wanted` chega, retoma sem nova partida.
- `DEFERRED`: a cada tick, `pruneStarts`; quando `count < maxStartsHour`, voltar a `IDLE`/`OPENING` p/ abrir as zonas desejadas (recomeça a sequência).
- `minOpen` não satisfeito no momento da partida → `GA_DEFER_MINOPEN` + `DEFERRED` (mesmo mecanismo).

Adicionar campos: `uint8_t closeFailStreak; uint32_t lastStartMs;` (lastStartMs já no header). `startRing`/`startCount`/`startHead` já previstos — usar `startHead` como índice de escrita.

- [ ] **Step 4: Rodar — passa**. **Step 5: Commit**

```bash
git add -A && git commit -m "feat(irrigation): engine — close-fail + bridging + max_partidas_hora (fase 7a)"
```

---

## Task 7: Engine — grupo sem bomba (simultaneidade) + reconciliação por heartbeat

**Files:**
- Modify: `src/modules/irrigation/HydraulicGroupEngine.cpp` (`observeActual`)
- Modify: `test/test_hydraulic_group_engine/test_main.cpp`

- [ ] **Step 1: Testes que falham**

```cpp
static void test_grupo_sem_bomba()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.bombaZoneId = 0; g.minOpen = 1; g.maxOpen = 2; // simultaneidade pura
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();
    e.setDesired(1, 1, true, 600);
    GroupEmit o1 = tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    TEST_ASSERT_EQUAL_UINT8(1, o1.zoneId);
    // sem bomba: nenhuma emissão de gpo de bomba; vai direto a RUNNING.
    tickNone(e, t, z, 6000); // não espera startAfterOpen p/ ligar bomba inexistente
    TEST_ASSERT_EQUAL(State::RUNNING, e.stateOf(1));
    TEST_ASSERT_FALSE(e.pumpOn(1));
}

static void test_reboot_reconcilia_desliga_bomba()
{
    ZoneTable z; seedZones(z);
    HydraulicGroup g = grp();
    g.minOpen = 1; g.maxOpen = 1;
    HydraulicGroupTable t; t.upsert(g);
    HydraulicGroupEngine e; e.reset();
    e.setDesired(1, 1, true, 600);
    tick1(e, t, z, 1000); e.noteSent(NODE, 1, 1, 1); e.onAck(NODE, 1);
    tick1(e, t, z, 6000); e.noteSent(NODE, 9, 1, 2); e.onAck(NODE, 2);
    TEST_ASSERT_TRUE(e.pumpOn(1));
    // estação reinicia: V1 caiu fora da orquestração -> heartbeat revela fechada.
    e.observeActual(1, 1, false); // actual-set cai < minOpen
    GroupEmit poff = tick1(e, t, z, 50000);
    TEST_ASSERT_EQUAL_UINT8(9, poff.zoneId);
    TEST_ASSERT_EQUAL_UINT8(0, poff.action); // parada ordenada da bomba
    HydraulicGroupEngine::GroupAlert al; bool got = false;
    while (e.takeAlert(al)) if (al.code == HydraulicGroupEngine::GA_REBOOT_RECONCILE) got = true;
    TEST_ASSERT_TRUE(got);
}
```

- [ ] **Step 2: Rodar — falha.**

- [ ] **Step 3: Implementar**

- Grupo sem bomba: em `START_WAIT`, se `cfg->bombaZoneId == 0` → `RUNNING` sem emitir nem esperar (já esboçado no Task 2; garantir que `OPENING`→`onAck` vá a `START_WAIT` e este resolva imediato). `pumpOn` fica false. Fim: `anyWanted()==false` → `DRAIN` sem `PUMP_OFF_WAIT`.
- `observeActual(groupId, zoneId, open)`: atualizar `ZoneRt.confirmed`. Se `open==false` e a zona era confirmada e o grupo está com bomba ligada, checar `confirmedCount(g) < cfg->minOpen`; se sim, marcar `g.reconcilePumpOff = true` (novo campo) — no `tick`, se `reconcilePumpOff` e `pump`, emitir bomba OFF (`PUMP_OFF_WAIT`), `pushAlert(GA_REBOOT_RECONCILE)`. Como `observeActual` não tem `cfg`, a checagem de `minOpen` também pode ir ao `tick`: em `RUNNING`/estados com pump, se `g.pump && confirmedCount(g) < cfg->minOpen` → parada ordenada + `GA_REBOOT_RECONCILE`. Implementar essa checagem no topo do `RUNNING` (cobre reboot e qualquer queda do actual-set).

- [ ] **Step 4: Rodar — passa**. **Step 5: Commit**

```bash
git add -A && git commit -m "feat(irrigation): engine — grupo sem bomba + reconciliacao por heartbeat (fase 7a)"
```

---

## Task 8: AuditOrigin + agregado + persistência (glue não-funcional)

**Files:**
- Modify: `src/modules/irrigation/AuditLog.h` (append origin)
- Modify: `src/modules/irrigation/IrrigationGateway.h` (membros)
- Modify: `src/modules/irrigation/IrrigationModule.h` (decls + path)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (loadGroups/saveGroups)

- [ ] **Step 1: Append `GRUPO_HIDRAULICO` ao enum**

`src/modules/irrigation/AuditLog.h`, no enum `AuditOrigin` (append no fim, valor 9):
```cpp
enum class AuditOrigin : uint8_t {
    SISTEMA = 0, CRONOGRAMA, PAINEL, PORTAL_CAMPO, BOTAO_FISICO,
    ENTRADA_FISICA, INTERTRAVAMENTO, FAILSAFE_TIMER, SERVICO,
    GRUPO_HIDRAULICO // = 9 (append-only ABI; rótulo JS chega no 7b)
};
```

- [ ] **Step 2: Adicionar membros ao agregado**

`src/modules/irrigation/IrrigationGateway.h`: adicionar include e membros:
```cpp
#include "modules/irrigation/HydraulicGroupTable.h"
#include "modules/irrigation/HydraulicGroupEngine.h"
// ... dentro do struct IrrigationGateway:
    HydraulicGroupTable groups;
    HydraulicGroupEngine groupEngine;
```

- [ ] **Step 3: Declarar path + métodos**

`src/modules/irrigation/IrrigationModule.h`: junto às outras decls de persistência do gateway, adicionar:
```cpp
    bool loadGroups();
    bool saveGroups();
```
E onde estão os `GW_*_PATH` (buscar `GW_INTERLOCKS_PATH` no `.cpp`), adicionar constantes `GW_GRUPOS_PATH`/`GW_GRUPOS_TMP` no mesmo local (arquivo `.cpp`, junto às demais).

- [ ] **Step 4: Implementar persistência**

`src/modules/irrigation/IrrigationModule.cpp`, após `saveInterlocks` (espelho exato), e definir os paths junto aos outros `GW_*_PATH`:
```cpp
// (junto às demais constantes de path)
static const char *GW_GRUPOS_PATH = "/irr/gw_grupos.bin";
static const char *GW_GRUPOS_TMP  = "/irr/gw_grupos.tmp";

bool IrrigationModule::loadGroups()
{
    size_t n = 0;
    uint8_t buf[6 + HydraulicGroupTable::MAX * 41 + 4];
    if (!stagedRead(GW_GRUPOS_PATH, buf, sizeof(buf), n))
        return false; // ausente na 1ª init — ok, tabela vazia
    return gateway.groups.deserialize(buf, n);
}

bool IrrigationModule::saveGroups()
{
    uint8_t buf[6 + HydraulicGroupTable::MAX * 41 + 4];
    size_t n = gateway.groups.serialize(buf, sizeof(buf));
    return stagedWrite(GW_GRUPOS_TMP, GW_GRUPOS_PATH, buf, n);
}
```
Chamar `loadGroups()` no init do gateway (onde `loadInterlocks()` é chamado — buscar `loadInterlocks(` no `.cpp` e adicionar `loadGroups();` ao lado).

- [ ] **Step 5: Verificar compilação (link do firmware)**

Run: `... ./bin/run-tests.sh -f test_hydraulic_group_table`
Expected: FILTERED (compila o firmware inteiro com os novos membros/persistência; link ok).

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "feat(irrigation): agregado + persistencia de grupos + AuditOrigin (fase 7a)"
```

---

## Task 9: Glue — roteamento no gwTick + wiring de ACK/falha/heartbeat

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (`gwSendValveCmd`, `gwTick`, `handleGwAck`, `handleGwHeartbeat`, ramo FAILED)
- Modify: `src/modules/irrigation/IrrigationModule.h` (assinatura de `gwSendValveCmd`)

- [ ] **Step 1: Expor o seq de `gwSendValveCmd`**

`IrrigationModule.h`: mudar a assinatura para devolver o seq usado:
```cpp
uint32_t gwSendValveCmd(uint32_t node, uint8_t index, uint8_t tipo, uint8_t action, uint16_t durationS,
                        uint8_t zoneId, uint8_t attempts);
```
`IrrigationModule.cpp:1557`: trocar `void` por `uint32_t`, capturar o seq e retorná-lo:
```cpp
uint32_t IrrigationModule::gwSendValveCmd(...)
{
    // ... monta pacote com ++txSeq ...
    uint32_t usedSeq = txSeq; // = o ++txSeq acabado de usar no encode
    if (!p->decoded.payload.size) { packetPool.release(p); return 0; }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    gateway.tracker.track(usedSeq, node, zoneId, action, durationS, attempts, millis());
    LOG_DEBUG(...);
    return usedSeq;
}
```
Os ~8 call-sites existentes ignoram o retorno (sem mudança neles — `uint32_t` descartado é válido).

- [ ] **Step 2: Rotear zonas agrupadas no scheduler drain**

Em `gwTick`, no drain do scheduler (`IrrigationModule.cpp:~1955-1991`), antes do tratamento OPEN/CLOSE atual, interceptar zonas agrupadas:
```cpp
const HydraulicGroup *hg = gateway.groups.byZone(a.zoneId);
if (hg) {
    if (a.type == SchedAction::Type::OPEN) {
        uint16_t dur = a.durationS;
        if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60)) dur = (uint16_t)(z->maxMin * 60);
        // interlock: zona bloqueada não entra no desejado
        if (gateway.interlockEngine.zoneVerdict(a.zoneId).bloqueada) {
            auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::CMD_REJEITADO, a.zoneId, AuditResult::NACK, z->node);
        } else {
            gateway.groupEngine.setDesired(hg->id, a.zoneId, true, dur);
        }
    } else { // CLOSE
        gateway.groupEngine.setDesired(hg->id, a.zoneId, false, 0);
    }
    continue; // não segue pelo caminho direto/openGate
}
// ... caminho atual (não-agrupado) inalterado
```

- [ ] **Step 3: Tickar o engine e enviar os emits**

Após o drain do scheduler e o dreno do openGate (fim do bloco de simultaneidade, `~linha 2009`), adicionar:
```cpp
// --- Fase 7a: motor de grupos hidráulicos ---
{
    GroupEmit emits[HydraulicGroupEngine::MAX_GROUPS * 2];
    size_t ne = gateway.groupEngine.tick(gateway.groups, gateway.zones, millis(), emits, sizeof(emits) / sizeof(emits[0]));
    for (size_t i = 0; i < ne; i++) {
        const GroupEmit &em = emits[i];
        const StationEntry *st = gateway.stations.byNode(em.node);
        uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
        uint32_t seq = gwSendValveCmd(em.node, em.index, em.tipo, em.action, em.durationS, em.zoneId, attempts);
        gateway.groupEngine.noteSent(em.node, em.zoneId, em.action, seq);
        auditEvent(AuditOrigin::GRUPO_HIDRAULICO, em.action ? AuditAction::ABRIR : AuditAction::FECHAR,
                   em.zoneId, AuditResult::OK, em.node);
    }
    // drena alertas do engine (AlertType::CMD_FAIL existe em StationMonitor.h; arg = código do grupo)
    HydraulicGroupEngine::GroupAlert ga;
    while (gateway.groupEngine.takeAlert(ga)) {
        Alert al; al.type = AlertType::CMD_FAIL; al.node = 0; al.arg = ga.code; al.atMs = millis();
        gateway.alerts.push(al);
        auditEvent(AuditOrigin::GRUPO_HIDRAULICO, AuditAction::CMD_REJEITADO, ga.zoneId, AuditResult::NACK, 0);
    }
}
```

- [ ] **Step 4: Hook `onAck`**

`handleGwAck` (`~2235`), após `gateway.tracker.onAck(...)` nos dois ramos, adicionar:
```cpp
gateway.groupEngine.onAck(mp.from, ack.ackedSeq);
```

- [ ] **Step 5: Hook `onCmdFailed`**

No ramo de retries do `gwTick` (`~2047`, `CommandTracker::Retry::What::FAILED`), após tratar o alerta atual, notificar o engine:
```cpp
if (r.what == CommandTracker::Retry::What::FAILED) {
    // ... alerta existente ...
    gateway.groupEngine.onCmdFailed(r.node, r.zoneId, r.action);
}
```

- [ ] **Step 6: Hook `observeActual` no heartbeat**

`handleGwHeartbeat` (`~2243`): ao processar o estado de válvulas/GPOs reportado, para cada zona daquela estação que pertence a um grupo, informar o engine do estado real:
```cpp
// após decodificar hb e localizar as zonas do nó mp.from:
for (size_t i = 0; i < gateway.zones.count(); i++) {
    const Zone *zz = gateway.zones.zoneAt(i);
    if (!zz || zz->node != mp.from) continue;
    const HydraulicGroup *hg = gateway.groups.byZone(zz->id);
    if (!hg) continue;
    // Heartbeat carrega bitmaps por índice de saída (IrrigationProtocol.h):
    //   uint8_t valveStates; uint8_t gpoStates;
    bool open = (zz->tipo == 1) ? ((hb.gpoStates >> zz->index) & 1)
                                : ((hb.valveStates >> zz->index) & 1);
    gateway.groupEngine.observeActual(hg->id, zz->id, open);
}
```

- [ ] **Step 7: Verificar compilação + suites de engine/tabela**

Run: `... ./bin/run-tests.sh -f test_hydraulic_group_engine`
Expected: FILTERED, PASSED (firmware inteiro compila com o glue; engine segue verde).

- [ ] **Step 8: Commit**

```bash
git add -A && git commit -m "feat(irrigation): glue — roteamento gwTick + ack/falha/heartbeat dos grupos (fase 7a)"
```

---

## Task 10: Registrar suites + verificação final GREEN

**Files:**
- Modify: `test/native-suite-count`

- [ ] **Step 1: Atualizar a contagem canônica**

`test/native-suite-count`: `54` → `56` (dois suites novos: `test_hydraulic_group_table`, `test_hydraulic_group_engine`).

- [ ] **Step 2: Rodar o suite COMPLETO**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh`
Expected: `RESULT: GREEN 56/56 suites passed [canonical: 56/56]` (exit 0). Se AMBER por count, reconferir o arquivo; se RED, ler a suite citada.

- [ ] **Step 3: Commit**

```bash
git add test/native-suite-count
git commit -m "test(irrigation): registra suites de grupos hidraulicos (56/56, fase 7a)"
```

- [ ] **Step 4: Marcar a fase no roadmap**

`docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md`, linha da Fase 7: anotar 7a concluída (data, plano, principais entregas) mantendo 7b como pendente. Commit:
```bash
git add docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "docs(irrigation): marca Fase 7a (motor de grupos) concluida no roadmap"
```

---

## Self-review checklist (o executor confirma ao fim)

- [ ] `trunk fmt` rodado (na CI; não instalado local) — sem drift de formatação.
- [ ] `./bin/run-tests.sh` GREEN 56/56.
- [ ] Nenhum call-site de `gwSendValveCmd` quebrado pela mudança de retorno.
- [ ] `AuditOrigin::GRUPO_HIDRAULICO` só append (não reordenou o enum).
- [ ] Zonas não-agrupadas seguem o caminho antigo intacto (openGate/interlock).
- [ ] Atualizar a memória de status (arquivo `irrigation-phase7a-status.md`) com follow-ups conhecidos e o gap do 7b (endpoints + UI).
```
