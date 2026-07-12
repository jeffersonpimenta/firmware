# Irrigação Mesh — Fase 4: Gateway (zonas, cronograma, retries, epoch, alertas, espelho)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** O gateway vira o cérebro da fazenda: registro de estações com reconciliação de epoch (incl. regra do maior epoch), tabela de zonas, cronograma semanal sequencial, comandos com retries/ACK e alerta de falha, monitor de saúde (bateria em níveis com histerese, estação muda, reboots anômalos) e **modo espelho 24VAC** (entradas físicas do gateway replicadas em saídas de qualquer estação, com bypass do cronograma — requisito do usuário).

**Architecture:** Seis componentes puros e testáveis (ZoneTable, StationRegistry, ProgramScheduler, CommandTracker, StationMonitor+AlertCenter, MirrorMode) que devolvem *ações* e nunca tocam rádio/FS; um agregado `IrrigationGateway` que os orquestra; o `IrrigationModule` (role GATEWAY) executa as ações (envia CMD/SET_CONFIG/GET_CONFIG, lê GPIO das entradas espelho, persiste tabelas com o padrão staged). UI de edição chega na Fase 5 — esta fase entrega o motor + persistência + defaults.

**Tech Stack:** C++17, Unity nativo em Docker (volumes `pio-cache`/`fwtest-cache`), componentes das Fases 1–3, `getValidTime(RTCQualityDevice, true)` (`src/gps/RTC.h`) para hora local do cronograma.

## Global Constraints

- **Retentativas de comando: 3** (padrão, por estação — spec §10); timeout de ACK **8 s** por tentativa; esgotou → alerta `CMD_FAIL`.
- **Alerta de silêncio**: sem heartbeat por `silencioAlertaMin` (padrão **35 min** = 3,5×HB, §10) → alerta com node id; dispara UMA vez até a estação voltar.
- **Bateria em níveis** (§8.1): aviso **12,2 V**, crítico **11,8 V**, hibernação **11,5 V**; **histerese 0,2 V** obrigatória em todos os limiares (sobe nível só acima de limiar+0,2 V).
- **Reboots anômalos** (§8.2): > **5 em 24 h** → alerta.
- **Epoch** (§5.4): `hbEpoch < desejado` → reenvia config automaticamente (blob de 52 B = 1 fragmento SET_CONFIG) até convergir (reenvio com cooldown de 30 s); **regra do maior epoch**: `hbEpoch > desejado` → `GET_CONFIG`, adota blob+epoch recebidos e registra alerta `CONFIG_ADOPTED` (auditoria plena na Fase 6).
- **Cronograma** (§7.1): programas semanais (máscara de dias, minuto de início), sequência de até 8 zonas com duração em minutos, execução **sequencial** (uma zona por vez), zonas clampadas a `maxMin` da zona; avanço por tempo (máquina guiada por ACK = grupos hidráulicos, Fase 7).
- **Espelho** (requisito do usuário 2026-07-11/12): entradas digitais do GATEWAY (pinos + polaridade de `pinsDigitalIn`/`digitalInActiveLow` do settings v3) mapeadas a zonas; enquanto entrada ativa → OPEN de **120 s renovado a cada 60 s** (fail-safe: gateway morto ⇒ estação fecha em ≤120 s); borda de queda → CLOSE; **bypass**: zona espelhada ignora cronograma e limites de zona (`maxMin` NÃO clampa o espelho; só o teto compilado de 120 min da estação vale); espelho ligável/desligável por flag persistida (UI na Fase 5).
- Zona = unidade lógica §5.2: `{id, nome, nó, tipo (valvula|gpo), índice, maxMin, padraoMin, fonteInput (-1 ou 0-3)}`. Estação §5.3: `{nó, nome, desiredEpoch, blob 52 B, retries, silencioAlertaMin, lat/lon}`.
- Limites: **16 estações, 24 zonas, 8 programas, 8 comandos pendentes, 32 alertas (ring)**.
- Persistência: mesmo padrão staged (`serialize`/`deserialize` com magic+versão, arquivos `/prefs/irrigation-{stations,zones,programs,mirror}.dat`).
- Sem regressão dos invariantes: `valves.tick` em todos os roles; módulo antes do RoutingModule; ACK⇔persistido⇔ativo; handlers de config em GATEWAY agora significam "resposta de GET_CONFIG → adoção" (nunca aplicar em si mesmo).
- Estilo: node IDs `0x%08x`; comentários mínimos; C++17; `#include "Arduino.h"` primeiro em todo test_main; nada em generated/.
- `test/native-suite-count` **38 → 43** (Task 6, nunca antes). **A suite completa é rodada pelo CONTROLLER, não pelo implementer** (implementer roda só as suites componentes).
- **RTC opcional (requisito do usuário 2026-07-12)**: a placa do gateway pode NÃO ter RTC. Sem hora válida (`getValidTime(...) == 0`): cronograma fica **inerte** (`ProgramScheduler::tick(0)` = no-op, já coberto por teste) com `LOG_WARN` 1×/h ("scheduler idle: no RTC"); **o modo espelho funciona normalmente** (usa só `millis()`, nunca hora de parede) — o sistema é 100% operacional só com espelhamento. Retries, alertas de silêncio/bateria e reconciliação de epoch também usam só `millis()` — nada além do cronograma depende de RTC.
- Template de teste (trocar `<SUITE>`/nome do container):

```
MSYS_NO_PATHCONV=1 docker run --rm --name irrig-p4tN -u 0 -e HOME=/root -v "$(pwd -W 2>/dev/null || pwd):/src:ro" -v pio-cache:/root -v fwtest-cache:/tmp/fw-test meshtastic-native-test timeout -k 30 1500 bash -c "cp -a /src/src/. /tmp/fw-test/src/ && cp -a /src/test/. /tmp/fw-test/test/ && cd /tmp/fw-test && platformio test -e coverage -f <SUITE> -vv 2>&1 | grep -E ':PASS|:FAIL|PASSED|FAILED|ERRORED|undefined|error' | head -40"
```

## File Structure

```
src/modules/irrigation/
  GatewayTables.h/.cpp     # Zone/StationEntry + ZoneTable + StationRegistry (Task 1)
  ProgramScheduler.h/.cpp  # cronograma semanal sequencial (Task 2)
  CommandTracker.h/.cpp    # pendências de ACK + retries (Task 3)
  StationMonitor.h/.cpp    # saúde por estação + AlertCenter (Task 4)
  MirrorMode.h/.cpp        # espelho 24VAC c/ debounce, renovação e bypass (Task 5)
  IrrigationGateway.h/.cpp # agregado puro de orquestração (Task 6)
  IrrigationModule.h/.cpp  # MODIFY: wiring GATEWAY (Task 6)
test/test_irrigation_gwtables/test_main.cpp   (Task 1)
test/test_irrigation_scheduler/test_main.cpp  (Task 2)
test/test_irrigation_cmdtracker/test_main.cpp (Task 3)
test/test_irrigation_monitor/test_main.cpp    (Task 4)
test/test_irrigation_mirror/test_main.cpp     (Task 5)
test/native-suite-count                       # 38 → 43 (Task 6)
```

---

### Task 1: `ZoneTable` + `StationRegistry` (`GatewayTables`)

**Files:**
- Create: `src/modules/irrigation/GatewayTables.h` / `.cpp`
- Test: `test/test_irrigation_gwtables/test_main.cpp`

**Interfaces:**
- Consumes: nada.
- Produces:

```cpp
struct Zone {
    uint8_t id = 0;          // 0 = slot vazio
    char name[16] = {0};
    uint32_t node = 0;
    uint8_t tipo = 0;        // 0 = valvula, 1 = gpo
    uint8_t index = 0;       // índice da saída na estação
    uint16_t maxMin = 120;
    uint16_t padraoMin = 20;
    int8_t fonteInput = -1;  // 0-3 = entrada espelho do gateway; -1 = nenhuma
};

class ZoneTable {
  public:
    static constexpr size_t MAX = 24;
    static constexpr uint32_t MAGIC = 0x495A4E31; // "IZN1"
    bool upsert(const Zone &z);          // por id (1..255); false = cheia
    bool removeById(uint8_t id);
    const Zone *byId(uint8_t id) const;  // nullptr = ausente
    const Zone *byFonte(int8_t input) const;
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // falha => tabela vazia
};

struct StationEntry {
    uint32_t node = 0;       // 0 = slot vazio
    char name[16] = {0};
    uint32_t desiredEpoch = 0;
    uint8_t blob[52] = {0};  // config v3 desejada (push §5.4)
    uint8_t retries = 3;
    uint16_t silencioAlertaMin = 35;
    int32_t lat = 0, lon = 0; // graus * 1e-5 (WGS84, §8.8)
};

class StationRegistry {
  public:
    static constexpr size_t MAX = 16;
    static constexpr uint32_t MAGIC = 0x49535431; // "IST1"
    bool upsert(const StationEntry &e);  // por node; false = cheio
    bool removeByNode(uint32_t node);
    const StationEntry *byNode(uint32_t node) const;
    StationEntry *mutableByNode(uint32_t node);
    size_t count() const;
    void adoptConfig(uint32_t node, const uint8_t *blob52, uint32_t epoch); // regra do maior epoch
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);
};
```

Serialização: `magic(4) + versão(1)=1 + count(1) + entradas fixas` (Zone = 28 B: id,name16,node4,tipo,index,maxMin2,padraoMin2,fonteInput; Station = 87 B: node4,name16,epoch4,blob52,retries1,silencio2,lat4,lon4). LE do host, mesmo padrão do Allowlist. `deserialize` valida magic/versão/count≤MAX/len exato.

- [ ] **Step 1: Testes que falham** — `test/test_irrigation_gwtables/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/GatewayTables.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static Zone mkZone(uint8_t id, uint32_t node, int8_t fonte = -1)
{
    Zone z;
    z.id = id;
    snprintf(z.name, sizeof(z.name), "Z%u", id);
    z.node = node;
    z.maxMin = 45;
    z.padraoMin = 20;
    z.fonteInput = fonte;
    return z;
}

static void test_zones_upsertByIdAndLookup()
{
    ZoneTable t;
    TEST_ASSERT_TRUE(t.upsert(mkZone(1, 0x11)));
    TEST_ASSERT_TRUE(t.upsert(mkZone(2, 0x22, 0)));
    TEST_ASSERT_EQUAL_UINT(2, t.count());

    Zone z1b = mkZone(1, 0x33); // mesmo id: substitui
    TEST_ASSERT_TRUE(t.upsert(z1b));
    TEST_ASSERT_EQUAL_UINT(2, t.count());
    TEST_ASSERT_EQUAL_HEX32(0x33, t.byId(1)->node);

    TEST_ASSERT_NOT_NULL(t.byFonte(0));
    TEST_ASSERT_EQUAL_UINT8(2, t.byFonte(0)->id);
    TEST_ASSERT_NULL(t.byFonte(3));
    TEST_ASSERT_NULL(t.byId(99));

    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_NULL(t.byId(1));
    TEST_ASSERT_FALSE(t.removeById(1));
}

static void test_zones_fullRejects_andIdZeroRejected()
{
    ZoneTable t;
    for (uint8_t i = 1; i <= ZoneTable::MAX; i++)
        TEST_ASSERT_TRUE(t.upsert(mkZone(i, i)));
    TEST_ASSERT_FALSE(t.upsert(mkZone(200, 200)));
    TEST_ASSERT_FALSE(t.upsert(mkZone(0, 1))); // id 0 = inválido
}

static void test_zones_serializeRoundTrip()
{
    ZoneTable t;
    t.upsert(mkZone(1, 0x11));
    t.upsert(mkZone(7, 0x77, 2));
    uint8_t buf[1024];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    ZoneTable c;
    TEST_ASSERT_TRUE(c.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(2, c.count());
    TEST_ASSERT_EQUAL_INT8(2, c.byId(7)->fonteInput);
    buf[0] ^= 0xFF;
    ZoneTable bad;
    TEST_ASSERT_FALSE(bad.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(0, bad.count());
}

static void test_stations_upsertAdoptAndRoundTrip()
{
    StationRegistry r;
    StationEntry e;
    e.node = 0xa1b2c3d4;
    snprintf(e.name, sizeof(e.name), "Pasto");
    e.desiredEpoch = 5;
    memset(e.blob, 0xAB, sizeof(e.blob));
    TEST_ASSERT_TRUE(r.upsert(e));
    TEST_ASSERT_EQUAL_UINT32(5, r.byNode(0xa1b2c3d4)->desiredEpoch);

    uint8_t newBlob[52];
    memset(newBlob, 0xCD, sizeof(newBlob));
    r.adoptConfig(0xa1b2c3d4, newBlob, 9); // regra do maior epoch
    TEST_ASSERT_EQUAL_UINT32(9, r.byNode(0xa1b2c3d4)->desiredEpoch);
    TEST_ASSERT_EQUAL_UINT8(0xCD, r.byNode(0xa1b2c3d4)->blob[0]);

    uint8_t buf[2048];
    size_t n = r.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    StationRegistry c;
    TEST_ASSERT_TRUE(c.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT32(9, c.byNode(0xa1b2c3d4)->desiredEpoch);
    TEST_ASSERT_EQUAL_STRING("Pasto", c.byNode(0xa1b2c3d4)->name);
}

static void test_stations_fullAndNodeZeroRejected()
{
    StationRegistry r;
    for (uint32_t i = 1; i <= StationRegistry::MAX; i++) {
        StationEntry e;
        e.node = i;
        TEST_ASSERT_TRUE(r.upsert(e));
    }
    StationEntry x;
    x.node = 999;
    TEST_ASSERT_FALSE(r.upsert(x));
    StationEntry z; // node 0
    TEST_ASSERT_FALSE(r.upsert(z));
    TEST_ASSERT_TRUE(r.removeByNode(3));
    TEST_ASSERT_TRUE(r.upsert(x));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_zones_upsertByIdAndLookup);
    RUN_TEST(test_zones_fullRejects_andIdZeroRejected);
    RUN_TEST(test_zones_serializeRoundTrip);
    RUN_TEST(test_stations_upsertAdoptAndRoundTrip);
    RUN_TEST(test_stations_fullAndNodeZeroRejected);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: RED** — suite `test_irrigation_gwtables`: FAIL de compilação.

- [ ] **Step 3: Implementar** — `GatewayTables.h` com os tipos acima e `GatewayTables.cpp`:

```cpp
#include "GatewayTables.h"
#include <string.h>

namespace
{
// serialização compartilhada: magic(4)+ver(1)+count(1)+entries
size_t writeHeader(uint8_t *buf, size_t cap, uint32_t magic, uint8_t count, size_t entrySize)
{
    size_t need = 6 + (size_t)count * entrySize;
    if (cap < need)
        return 0;
    memcpy(buf, &magic, 4);
    buf[4] = 1;
    buf[5] = count;
    return need;
}
bool checkHeader(const uint8_t *buf, size_t n, uint32_t magic, size_t entrySize, uint8_t maxCount, uint8_t &countOut)
{
    if (n < 6)
        return false;
    uint32_t m;
    memcpy(&m, buf, 4);
    if (m != magic || buf[4] != 1)
        return false;
    countOut = buf[5];
    return countOut <= maxCount && n == 6 + (size_t)countOut * entrySize;
}
} // namespace

// ---- ZoneTable ----
static constexpr size_t ZONE_ENTRY = 28;

bool ZoneTable::upsert(const Zone &z)
{
    if (z.id == 0)
        return false;
    for (auto &s : zones)
        if (s.id == z.id) {
            s = z;
            return true;
        }
    for (auto &s : zones)
        if (s.id == 0) {
            s = z;
            return true;
        }
    return false;
}

bool ZoneTable::removeById(uint8_t id)
{
    for (auto &s : zones)
        if (s.id == id && id != 0) {
            s = Zone{};
            return true;
        }
    return false;
}

const Zone *ZoneTable::byId(uint8_t id) const
{
    if (id == 0)
        return nullptr;
    for (auto &s : zones)
        if (s.id == id)
            return &s;
    return nullptr;
}

const Zone *ZoneTable::byFonte(int8_t input) const
{
    if (input < 0)
        return nullptr;
    for (auto &s : zones)
        if (s.id != 0 && s.fonteInput == input)
            return &s;
    return nullptr;
}

size_t ZoneTable::count() const
{
    size_t c = 0;
    for (auto &s : zones)
        if (s.id != 0)
            c++;
    return c;
}

size_t ZoneTable::serialize(uint8_t *buf, size_t cap) const
{
    uint8_t cnt = (uint8_t)count();
    size_t need = writeHeader(buf, cap, MAGIC, cnt, ZONE_ENTRY);
    if (!need)
        return 0;
    size_t off = 6;
    for (auto &z : zones) {
        if (z.id == 0)
            continue;
        buf[off] = z.id;
        memcpy(buf + off + 1, z.name, 16);
        memcpy(buf + off + 17, &z.node, 4);
        buf[off + 21] = z.tipo;
        buf[off + 22] = z.index;
        memcpy(buf + off + 23, &z.maxMin, 2);
        memcpy(buf + off + 25, &z.padraoMin, 2);
        buf[off + 27] = (uint8_t)z.fonteInput;
        off += ZONE_ENTRY;
    }
    return need;
}

bool ZoneTable::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : zones)
        s = Zone{};
    uint8_t cnt;
    if (!checkHeader(buf, n, MAGIC, ZONE_ENTRY, MAX, cnt))
        return false;
    size_t off = 6;
    for (uint8_t i = 0; i < cnt; i++, off += ZONE_ENTRY) {
        Zone z;
        z.id = buf[off];
        memcpy(z.name, buf + off + 1, 16);
        z.name[15] = '\0';
        memcpy(&z.node, buf + off + 17, 4);
        z.tipo = buf[off + 21];
        z.index = buf[off + 22];
        memcpy(&z.maxMin, buf + off + 23, 2);
        memcpy(&z.padraoMin, buf + off + 25, 2);
        z.fonteInput = (int8_t)buf[off + 27];
        zones[i] = z;
    }
    return true;
}
```

(StationRegistry análogo: `STATION_ENTRY = 87`; campos na ordem node4,name16,epoch4,blob52,retries1,silencio2,lat4,lon4; `adoptConfig` = `mutableByNode` → memcpy blob + epoch, no-op se nó ausente. Membros privados: `Zone zones[MAX];` / `StationEntry stations[MAX];`.)

- [ ] **Step 4: GREEN** — suite `test_irrigation_gwtables`: PASS (5 testes).
- [ ] **Step 5: Commit** — `feat(irrigation): add gateway zone table and station registry`

---

### Task 2: `ProgramScheduler`

**Files:**
- Create: `src/modules/irrigation/ProgramScheduler.h` / `.cpp`
- Test: `test/test_irrigation_scheduler/test_main.cpp`

**Interfaces:**
- Consumes: nada (zonas resolvidas pelo glue).
- Produces:

```cpp
struct Program {
    uint8_t id = 0; // 0 = vazio
    bool enabled = true;
    uint8_t daysMask = 0;    // bit0 = domingo … bit6 = sábado
    uint16_t startMinute = 0; // minuto do dia local (0..1439)
    uint8_t stepCount = 0;
    struct Step {
        uint8_t zoneId = 0;
        uint16_t durationMin = 0;
    } steps[8];
};

struct SchedAction {
    enum class Type : uint8_t { NONE, OPEN, CLOSE } type = Type::NONE;
    uint8_t zoneId = 0;
    uint16_t durationS = 0;
};

class ProgramScheduler {
  public:
    static constexpr size_t MAX_PROGRAMS = 8;
    static constexpr uint32_t MAGIC = 0x49505231; // "IPR1"
    bool upsert(const Program &p);
    bool removeById(uint8_t id);
    size_t count() const;
    // Chamar 1×/s com hora local válida (epochLocalSecs != 0). Devolve UMA ação
    // por chamada; chamar de novo no mesmo segundo até NONE.
    SchedAction tick(uint32_t epochLocalSecs);
    bool running() const { return activeProgram != 0; }
    void abort(); // glue fecha a zona corrente antes (usa currentZone())
    uint8_t currentZone() const { return running() ? curZone : 0; }
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);
};
```

Semântica: dispara quando `weekday ∈ daysMask` e `minuteOfDay == startMinute` (uma vez por minuto — guarda `lastFireKey = epoch/60`); um programa por vez (se um está ativo, início de outro é ignorado — alerta fica p/ o glue via retorno? não: simplesmente ignora; glue não precisa saber); sequência: OPEN(step0) → quando `durationMin` passa → CLOSE(step0) + OPEN(step1) em ticks sucessivos → … → CLOSE(último) e volta a idle. `weekday = ((epochLocalSecs / 86400) + 4) % 7` (1970-01-01 = quinta = 4). durationS = durationMin*60 (clamp de `maxMin` da zona = glue).

- [ ] **Step 1: Testes que falham** — `test/test_irrigation_scheduler/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ProgramScheduler.h"
#include <unity.h>

using T = SchedAction::Type;

void setUp(void) {}
void tearDown(void) {}

// Segunda-feira 1970-01-05 00:00 UTC = epoch 345600. bit segunda = 1<<1.
static constexpr uint32_t MONDAY = 345600;

static Program mkProg(uint8_t id, uint16_t startMin, uint8_t nSteps)
{
    Program p;
    p.id = id;
    p.daysMask = 1 << 1; // segunda
    p.startMinute = startMin;
    p.stepCount = nSteps;
    for (uint8_t i = 0; i < nSteps; i++) {
        p.steps[i].zoneId = 10 + i;
        p.steps[i].durationMin = 2;
    }
    return p;
}

static SchedAction drain(ProgramScheduler &s, uint32_t t, SchedAction *extra = nullptr)
{
    SchedAction a = s.tick(t);
    if (extra)
        *extra = s.tick(t);
    else
        s.tick(t);
    return a;
}

static void test_firesOnDayAndMinute_runsSequence()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 2)); // segunda 08:00, 2 zonas de 2 min

    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 479 * 60)); // 07:59
    SchedAction a = s.tick(MONDAY + 480 * 60);             // 08:00
    TEST_ASSERT_EQUAL(T::OPEN, a.type);
    TEST_ASSERT_EQUAL_UINT8(10, a.zoneId);
    TEST_ASSERT_EQUAL_UINT16(120, a.durationS);
    TEST_ASSERT_TRUE(s.running());

    // 2 min depois: fecha z10, abre z11 (dois ticks)
    SchedAction c = s.tick(MONDAY + 482 * 60);
    TEST_ASSERT_EQUAL(T::CLOSE, c.type);
    TEST_ASSERT_EQUAL_UINT8(10, c.zoneId);
    SchedAction o = s.tick(MONDAY + 482 * 60);
    TEST_ASSERT_EQUAL(T::OPEN, o.type);
    TEST_ASSERT_EQUAL_UINT8(11, o.zoneId);

    // +2 min: fecha z11 e termina
    SchedAction f = s.tick(MONDAY + 484 * 60);
    TEST_ASSERT_EQUAL(T::CLOSE, f.type);
    TEST_ASSERT_EQUAL_UINT8(11, f.zoneId);
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 484 * 60));
    TEST_ASSERT_FALSE(s.running());
}

static void test_firesOncePerMinute_notOnWrongDay()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 1));
    TEST_ASSERT_EQUAL(T::OPEN, s.tick(MONDAY + 480 * 60).type);
    s.abort();
    // mesmo minuto de novo: não redispara
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 480 * 60 + 30).type);
    // terça, mesmo horário: fora da máscara
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 86400 + 480 * 60).type);
}

static void test_secondProgramIgnoredWhileRunning()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 1));
    Program p2 = mkProg(2, 481, 1);
    p2.steps[0].zoneId = 99;
    s.upsert(p2);
    TEST_ASSERT_EQUAL(T::OPEN, s.tick(MONDAY + 480 * 60).type);
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 481 * 60).type); // p2 ignorado
    TEST_ASSERT_TRUE(s.running());
}

static void test_disabledAndZeroTimeIgnored()
{
    ProgramScheduler s;
    Program p = mkProg(1, 480, 1);
    p.enabled = false;
    s.upsert(p);
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 480 * 60).type);
    TEST_ASSERT_EQUAL(T::NONE, s.tick(0).type); // sem RTC válido: nada dispara
}

static void test_abortStopsRun()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 2));
    s.tick(MONDAY + 480 * 60);
    TEST_ASSERT_EQUAL_UINT8(10, s.currentZone());
    s.abort();
    TEST_ASSERT_FALSE(s.running());
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 482 * 60).type);
}

static void test_serializeRoundTrip()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 2));
    s.upsert(mkProg(3, 1200, 1));
    uint8_t buf[512];
    size_t n = s.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    ProgramScheduler c;
    TEST_ASSERT_TRUE(c.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(2, c.count());
    TEST_ASSERT_EQUAL(T::OPEN, c.tick(MONDAY + 480 * 60).type);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_firesOnDayAndMinute_runsSequence);
    RUN_TEST(test_firesOncePerMinute_notOnWrongDay);
    RUN_TEST(test_secondProgramIgnoredWhileRunning);
    RUN_TEST(test_disabledAndZeroTimeIgnored);
    RUN_TEST(test_abortStopsRun);
    RUN_TEST(test_serializeRoundTrip);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: RED**; **Step 3: Implementar**:

```cpp
#include "ProgramScheduler.h"
#include <string.h>

namespace
{
constexpr size_t PROG_ENTRY = 1 + 1 + 1 + 2 + 1 + 8 * 3; // 30 B
} // namespace

bool ProgramScheduler::upsert(const Program &p)
{
    if (p.id == 0 || p.stepCount > 8)
        return false;
    for (auto &s : programs)
        if (s.id == p.id) {
            s = p;
            return true;
        }
    for (auto &s : programs)
        if (s.id == 0) {
            s = p;
            return true;
        }
    return false;
}

bool ProgramScheduler::removeById(uint8_t id)
{
    for (auto &s : programs)
        if (s.id == id && id != 0) {
            if (activeProgram == id)
                abort();
            s = Program{};
            s.id = 0;
            return true;
        }
    return false;
}

size_t ProgramScheduler::count() const
{
    size_t c = 0;
    for (auto &s : programs)
        if (s.id != 0)
            c++;
    return c;
}

void ProgramScheduler::abort()
{
    activeProgram = 0;
    curStep = 0;
    curZone = 0;
    pendingOpenNext = false;
}

SchedAction ProgramScheduler::tick(uint32_t epochLocalSecs)
{
    SchedAction none;
    if (epochLocalSecs == 0)
        return none;

    if (activeProgram != 0) {
        const Program *p = nullptr;
        for (auto &s : programs)
            if (s.id == activeProgram)
                p = &s;
        if (!p) {
            abort();
            return none;
        }
        if (pendingOpenNext) { // segunda metade da transição CLOSE→OPEN
            pendingOpenNext = false;
            curZone = p->steps[curStep].zoneId;
            stepEndSecs = epochLocalSecs + (uint32_t)p->steps[curStep].durationMin * 60;
            return {SchedAction::Type::OPEN, curZone, (uint16_t)(p->steps[curStep].durationMin * 60)};
        }
        if (epochLocalSecs >= stepEndSecs) {
            SchedAction close{SchedAction::Type::CLOSE, curZone, 0};
            if (curStep + 1 < p->stepCount) {
                curStep++;
                pendingOpenNext = true;
            } else {
                abort();
            }
            return close;
        }
        return none;
    }

    uint32_t minuteKey = epochLocalSecs / 60;
    if (minuteKey == lastFireKey)
        return none;
    uint8_t weekday = (uint8_t)(((epochLocalSecs / 86400) + 4) % 7);
    uint16_t minuteOfDay = (uint16_t)((epochLocalSecs % 86400) / 60);
    for (auto &p : programs) {
        if (p.id == 0 || !p.enabled || p.stepCount == 0)
            continue;
        if (!(p.daysMask & (1u << weekday)) || p.startMinute != minuteOfDay)
            continue;
        lastFireKey = minuteKey;
        activeProgram = p.id;
        curStep = 0;
        curZone = p.steps[0].zoneId;
        stepEndSecs = epochLocalSecs + (uint32_t)p.steps[0].durationMin * 60;
        return {SchedAction::Type::OPEN, curZone, (uint16_t)(p.steps[0].durationMin * 60)};
    }
    return none;
}
```

Membros privados: `Program programs[MAX_PROGRAMS]; uint8_t activeProgram = 0; uint8_t curStep = 0; uint8_t curZone = 0; uint32_t stepEndSecs = 0; uint32_t lastFireKey = 0; bool pendingOpenNext = false;`. Serialize/deserialize no padrão do Task 1 (magic IPR1, entrada 30 B: id,enabled,daysMask,startMinute2,stepCount, 8×(zoneId,durationMin2)).

Nota ao executor: `durationS` em `uint16_t` limita passo a 65535 s ≈ 18 h — durationMin ≤ 1092; clamp no upsert: `durationMin > 720 → 720` (12 h, acima do teto da estação de qualquer forma).

- [ ] **Step 4: GREEN** (6 testes). **Step 5: Commit** — `feat(irrigation): add weekly program scheduler`

---

### Task 3: `CommandTracker`

**Files:**
- Create: `src/modules/irrigation/CommandTracker.h` / `.cpp`
- Test: `test/test_irrigation_cmdtracker/test_main.cpp`

**Interfaces:**

```cpp
class CommandTracker {
  public:
    static constexpr size_t MAX_PENDING = 8;
    static constexpr uint32_t ACK_TIMEOUT_MS = 8000;

    struct Retry {
        enum class What : uint8_t { NONE, RESEND, FAILED } what = What::NONE;
        uint32_t node = 0;
        uint8_t zoneId = 0;
        uint8_t action = 0;      // 0 fechar, 1 abrir
        uint16_t durationS = 0;
        uint8_t attemptsLeft = 0;
    };

    bool track(uint32_t seq, uint32_t node, uint8_t zoneId, uint8_t action, uint16_t durationS, uint8_t attempts,
               uint32_t nowMs); // false = fila cheia
    bool onAck(uint32_t node, uint32_t ackedSeq); // true = pendência resolvida
    void retrack(uint32_t newSeq, const Retry &r, uint32_t nowMs); // após reenvio
    Retry poll(uint32_t nowMs); // 1 por chamada: RESEND (removeu; chamar retrack com novo seq) ou FAILED (esgotou)
};
```

Semântica: `track` registra; `onAck(node,seq)` remove a pendência casada; `poll` varre timeouts (`nowMs - sentAt >= 8000`): se `attemptsLeft > 1` devolve RESEND com `attemptsLeft-1` e REMOVE (o glue reenvia com seq novo e chama `retrack`); se `attemptsLeft == 1` devolve FAILED e remove (glue alerta).

- [ ] **Step 1: Testes que falham** — `test/test_irrigation_cmdtracker/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/CommandTracker.h"
#include <unity.h>

using What = CommandTracker::Retry::What;

void setUp(void) {}
void tearDown(void) {}

static void test_ackClearsPending()
{
    CommandTracker t;
    TEST_ASSERT_TRUE(t.track(100, 0x11, 1, 1, 600, 3, 1000));
    TEST_ASSERT_TRUE(t.onAck(0x11, 100));
    TEST_ASSERT_FALSE(t.onAck(0x11, 100)); // já resolvido
    TEST_ASSERT_EQUAL(What::NONE, t.poll(20000).what);
}

static void test_timeoutYieldsResendThenFailed()
{
    CommandTracker t;
    t.track(100, 0x11, 1, 1, 600, 2, 1000);
    TEST_ASSERT_EQUAL(What::NONE, t.poll(8999).what); // 7.999 s
    auto r = t.poll(9000);                            // 8 s
    TEST_ASSERT_EQUAL(What::RESEND, r.what);
    TEST_ASSERT_EQUAL_UINT8(1, r.attemptsLeft);
    TEST_ASSERT_EQUAL_HEX32(0x11, r.node);

    t.retrack(101, r, 9000);
    auto f = t.poll(17000); // +8 s
    TEST_ASSERT_EQUAL(What::FAILED, f.what);
    TEST_ASSERT_EQUAL_UINT8(1, f.zoneId);
    TEST_ASSERT_EQUAL(What::NONE, t.poll(30000).what);
}

static void test_wrongNodeOrSeqDoesNotClear()
{
    CommandTracker t;
    t.track(100, 0x11, 1, 1, 600, 3, 1000);
    TEST_ASSERT_FALSE(t.onAck(0x22, 100));
    TEST_ASSERT_FALSE(t.onAck(0x11, 999));
    TEST_ASSERT_EQUAL(What::RESEND, t.poll(9001).what);
}

static void test_queueFullRejects()
{
    CommandTracker t;
    for (uint32_t i = 0; i < CommandTracker::MAX_PENDING; i++)
        TEST_ASSERT_TRUE(t.track(100 + i, i + 1, 1, 1, 60, 3, 0));
    TEST_ASSERT_FALSE(t.track(200, 99, 1, 1, 60, 3, 0));
    TEST_ASSERT_TRUE(t.onAck(1, 100));
    TEST_ASSERT_TRUE(t.track(200, 99, 1, 1, 60, 3, 0));
}

static void test_rolloverSafe()
{
    CommandTracker t;
    t.track(100, 0x11, 1, 1, 60, 2, 0xFFFFF000u);
    TEST_ASSERT_EQUAL(What::NONE, t.poll(0xFFFFF000u + 7000).what);
    TEST_ASSERT_EQUAL(What::RESEND, t.poll(0xFFFFF000u + 8000).what); // atravessa o wrap
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_ackClearsPending);
    RUN_TEST(test_timeoutYieldsResendThenFailed);
    RUN_TEST(test_wrongNodeOrSeqDoesNotClear);
    RUN_TEST(test_queueFullRejects);
    RUN_TEST(test_rolloverSafe);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: RED**; **Step 3: Implementar** (array fixo `struct P { bool inUse; uint32_t seq, node, sentAtMs; uint8_t zoneId, action, attemptsLeft; uint16_t durationS; } slots[MAX_PENDING];` — `poll` usa `(nowMs - sentAtMs) >= ACK_TIMEOUT_MS` unsigned; RESEND remove o slot e devolve `attemptsLeft - 1`).

- [ ] **Step 4: GREEN** (5 testes). **Step 5: Commit** — `feat(irrigation): add command ack tracker with retries`

---

### Task 4: `StationMonitor` + `AlertCenter`

**Files:**
- Create: `src/modules/irrigation/StationMonitor.h` / `.cpp`
- Test: `test/test_irrigation_monitor/test_main.cpp`

**Interfaces:**

```cpp
enum class AlertType : uint8_t {
    NONE = 0, BATT_AVISO, BATT_CRITICO, BATT_HIBERNACAO, BATT_RECUPEROU,
    SILENT, BACK_ONLINE, REBOOT_ANOMALY, CMD_FAIL, CONFIG_ADOPTED
};
struct Alert { AlertType type = AlertType::NONE; uint32_t node = 0; uint32_t arg = 0; uint32_t atMs = 0; };

class AlertCenter {
  public:
    static constexpr size_t MAX = 32;
    void push(const Alert &a); // ring: sobrescreve o mais antigo
    size_t count() const;      // total ainda armazenado (≤32)
    const Alert &at(size_t i) const; // 0 = mais recente
};

class StationMonitor {
  public:
    static constexpr size_t MAX = 16;
    static constexpr uint16_t HYST_CV = 20; // 0,2 V
    static constexpr uint16_t AVISO_CV = 1220, CRITICO_CV = 1180, HIBER_CV = 1150;
    static constexpr uint16_t REBOOT_LIMIT_24H = 5;
    // devolve até 3 alertas por chamada (volta de silêncio + nível de bateria + reboot) via out; retorna quantos
    int onHeartbeat(uint32_t node, uint16_t vbatCentiV, uint16_t rebootCount, uint32_t nowMs, Alert out[3]);
    // varredura de silêncio: chamar 1×/s por estação registrada; SILENT dispara 1× até voltar
    bool checkSilence(uint32_t node, uint32_t silencioMs, uint32_t nowMs, Alert &out);
    uint32_t lastHeardMs(uint32_t node) const; // 0 = nunca
};
```

Regras: nível de bateria com histerese — desce imediatamente ao cruzar limiar p/ baixo (gera alerta do novo nível), só sobe de nível quando `vbat >= limiar_do_nível_acima + 20 cV` (subida acima de AVISO+20 gera `BATT_RECUPEROU`); um alerta por MUDANÇA de nível, nunca repetido no mesmo nível. Reboot: janela deslizante simplificada — guarda `rebootCount` no primeiro HB e o timestamp; se `rebootCount - baseline > 5` dentro de 24 h → `REBOOT_ANOMALY` (1× por janela); janela renova a cada 24 h. `checkSilence`: `everHeard && nowMs-lastHeard > silencioMs` → SILENT 1×; próximo HB → `BACK_ONLINE`.

- [ ] **Step 1: Testes que falham** — `test/test_irrigation_monitor/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/StationMonitor.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_batteryLevelsWithHysteresis()
{
    StationMonitor m;
    Alert out[3];
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1250, 0, 1000, out)); // normal
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1210, 0, 2000, out)); // < 12,2
    TEST_ASSERT_EQUAL(AlertType::BATT_AVISO, out[0].type);
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1215, 0, 3000, out)); // dentro da histerese: nada
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1170, 0, 4000, out)); // < 11,8
    TEST_ASSERT_EQUAL(AlertType::BATT_CRITICO, out[0].type);
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1140, 0, 5000, out)); // < 11,5
    TEST_ASSERT_EQUAL(AlertType::BATT_HIBERNACAO, out[0].type);
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1190, 0, 6000, out)); // subiu mas < crítico+hist? 1190 >= 1180+20? não (1200) → continua hibern.? sobe p/ crítico? ver regra
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1250, 0, 7000, out)); // >= 1220+20 → recuperou
    TEST_ASSERT_EQUAL(AlertType::BATT_RECUPEROU, out[0].type);
}

static void test_silenceFiresOnceAndBackOnline()
{
    StationMonitor m;
    Alert out[3], a;
    m.onHeartbeat(0x11, 1250, 0, 1000, out);
    TEST_ASSERT_FALSE(m.checkSilence(0x11, 60000, 50000, a));
    TEST_ASSERT_TRUE(m.checkSilence(0x11, 60000, 62000, a));
    TEST_ASSERT_EQUAL(AlertType::SILENT, a.type);
    TEST_ASSERT_FALSE(m.checkSilence(0x11, 60000, 70000, a)); // 1× só
    int n = m.onHeartbeat(0x11, 1250, 0, 80000, out);         // voltou
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(AlertType::BACK_ONLINE, out[0].type);
}

static void test_silenceNeverHeardDoesNotFire()
{
    StationMonitor m;
    Alert a;
    TEST_ASSERT_FALSE(m.checkSilence(0x99, 60000, 100000, a));
}

static void test_rebootAnomaly()
{
    StationMonitor m;
    Alert out[3];
    m.onHeartbeat(0x11, 1250, 10, 1000, out);                          // baseline 10
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1250, 14, 2000, out)); // +4
    TEST_ASSERT_EQUAL_INT(1, m.onHeartbeat(0x11, 1250, 16, 3000, out)); // +6 > 5
    TEST_ASSERT_EQUAL(AlertType::REBOOT_ANOMALY, out[0].type);
    TEST_ASSERT_EQUAL_INT(0, m.onHeartbeat(0x11, 1250, 18, 4000, out)); // 1× por janela
}

static void test_alertRing()
{
    AlertCenter c;
    for (uint32_t i = 0; i < 40; i++)
        c.push({AlertType::SILENT, i, 0, i});
    TEST_ASSERT_EQUAL_UINT(AlertCenter::MAX, c.count());
    TEST_ASSERT_EQUAL_UINT32(39, c.at(0).node);                    // mais recente
    TEST_ASSERT_EQUAL_UINT32(8, c.at(AlertCenter::MAX - 1).node);  // mais antigo restante
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_batteryLevelsWithHysteresis);
    RUN_TEST(test_silenceFiresOnceAndBackOnline);
    RUN_TEST(test_silenceNeverHeardDoesNotFire);
    RUN_TEST(test_rebootAnomaly);
    RUN_TEST(test_alertRing);
    exit(UNITY_END());
}

void loop() {}
```

Nota ao executor: no teste de histerese, a chamada com 1190 cV documenta a decisão: subida SÓ acontece quando cruza `nível_acima + HYST`; de hibernação (nível 3), 1190 < 1180+20 → permanece, sem alerta. A recuperação total (≥ 1240) emite apenas `BATT_RECUPEROU` (não emite os níveis intermediários).

- [ ] **Step 2: RED**; **Step 3: Implementar** (por estação: `{node, lastHeardMs, level 0-3, silentFired, rebootBase, rebootWindowStartMs, anomalyFired, everHeard}`; nível calculado por limiares descendo, `level+HYST` subindo; AlertCenter = ring com índice de escrita).

- [ ] **Step 4: GREEN** (5 testes). **Step 5: Commit** — `feat(irrigation): add station health monitor and alert ring`

---

### Task 5: `MirrorMode`

**Files:**
- Create: `src/modules/irrigation/MirrorMode.h` / `.cpp`
- Test: `test/test_irrigation_mirror/test_main.cpp`

**Interfaces:**

```cpp
class MirrorMode {
  public:
    static constexpr uint16_t OPEN_S = 120;     // duração de cada comando (fail-safe ≤120 s)
    static constexpr uint32_t RENEW_MS = 60000; // renovação (§4.2) enquanto entrada ativa
    static constexpr uint32_t DEBOUNCE_MS = 100;
    static constexpr size_t INPUTS = 4;
    static constexpr uint32_t MAGIC = 0x494D5231; // "IMR1"

    struct Action {
        enum class T : uint8_t { NONE, OPEN, CLOSE } t = T::NONE;
        uint8_t input = 0; // qual entrada causou (zona = glue via ZoneTable::byFonte)
    };

    void setEnabled(bool e);
    bool enabled() const;
    // bitmap já com polaridade aplicada (bit i = entrada i ativa). 1 ação por
    // chamada; chamar até NONE. OPEN repete a cada RENEW_MS enquanto ativa.
    Action update(uint8_t inputsBitmap, uint32_t nowMs);
    bool inputActive(uint8_t input) const; // pós-debounce (p/ bypass do cronograma)
    size_t serialize(uint8_t *buf, size_t cap) const;   // só o flag enabled
    bool deserialize(const uint8_t *buf, size_t n);
};
```

Semântica: debounce de 100 ms por entrada (mudança precisa ficar estável); borda de subida → OPEN(input) e marca `lastRenewMs`; enquanto ativa e `nowMs - lastRenew >= 60000` → OPEN de novo (renovação); borda de queda → CLOSE(input). `enabled == false` → `update` devolve CLOSE para qualquer entrada ainda ativa (drena) e depois NONE sempre. Persistência: `magic(4)+ver(1)+enabled(1)`.

- [ ] **Step 1: Testes que falham** — `test/test_irrigation_mirror/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/MirrorMode.h"
#include <unity.h>

using T = MirrorMode::Action::T;

void setUp(void) {}
void tearDown(void) {}

// leva o debounce: aplica bitmap por >=100 ms com chamadas a cada 25 ms
static MirrorMode::Action settle(MirrorMode &m, uint8_t bm, uint32_t from)
{
    MirrorMode::Action last;
    for (uint32_t t = from; t <= from + 125; t += 25) {
        auto a = m.update(bm, t);
        if (a.t != T::NONE)
            last = a;
    }
    return last;
}

static void test_riseOpensFallCloses()
{
    MirrorMode m;
    m.setEnabled(true);
    auto a = settle(m, 0b0001, 1000);
    TEST_ASSERT_EQUAL(T::OPEN, a.t);
    TEST_ASSERT_EQUAL_UINT8(0, a.input);
    TEST_ASSERT_TRUE(m.inputActive(0));
    auto c = settle(m, 0b0000, 5000);
    TEST_ASSERT_EQUAL(T::CLOSE, c.t);
    TEST_ASSERT_FALSE(m.inputActive(0));
}

static void test_renewalEvery60s()
{
    MirrorMode m;
    m.setEnabled(true);
    settle(m, 0b0010, 1000);
    TEST_ASSERT_EQUAL(T::NONE, m.update(0b0010, 30000).t);
    auto r = m.update(0b0010, 1000 + 125 + 60000);
    TEST_ASSERT_EQUAL(T::OPEN, r.t); // renovação
    TEST_ASSERT_EQUAL_UINT8(1, r.input);
}

static void test_glitchIgnored()
{
    MirrorMode m;
    m.setEnabled(true);
    m.update(0b0001, 1000);
    m.update(0b0000, 1050); // caiu antes de 100 ms
    TEST_ASSERT_EQUAL(T::NONE, m.update(0b0000, 1200).t);
    TEST_ASSERT_FALSE(m.inputActive(0));
}

static void test_disableDrainsActiveInputs()
{
    MirrorMode m;
    m.setEnabled(true);
    settle(m, 0b0101, 1000); // pode vir OPEN de 0 e 2 em chamadas sucessivas
    settle(m, 0b0101, 2000);
    m.setEnabled(false);
    auto c1 = m.update(0b0101, 3000);
    auto c2 = m.update(0b0101, 3000);
    bool closed0 = (c1.t == T::CLOSE && c1.input == 0) || (c2.t == T::CLOSE && c2.input == 0);
    bool closed2 = (c1.t == T::CLOSE && c1.input == 2) || (c2.t == T::CLOSE && c2.input == 2);
    TEST_ASSERT_TRUE(closed0);
    TEST_ASSERT_TRUE(closed2);
    TEST_ASSERT_EQUAL(T::NONE, m.update(0b0101, 3100).t); // desabilitado: ignora entradas
}

static void test_twoInputsIndependent()
{
    MirrorMode m;
    m.setEnabled(true);
    auto a = settle(m, 0b1000, 1000);
    TEST_ASSERT_EQUAL_UINT8(3, a.input);
    // entra a 0 também; 3 continua
    MirrorMode::Action b = settle(m, 0b1001, 2000);
    TEST_ASSERT_EQUAL(T::OPEN, b.t);
    TEST_ASSERT_EQUAL_UINT8(0, b.input);
    TEST_ASSERT_TRUE(m.inputActive(3));
    auto c = settle(m, 0b0001, 4000); // solta 3
    TEST_ASSERT_EQUAL(T::CLOSE, c.t);
    TEST_ASSERT_EQUAL_UINT8(3, c.input);
    TEST_ASSERT_TRUE(m.inputActive(0));
}

static void test_serializeEnabledFlag()
{
    MirrorMode m;
    m.setEnabled(true);
    uint8_t buf[8];
    size_t n = m.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    MirrorMode c;
    TEST_ASSERT_TRUE(c.deserialize(buf, n));
    TEST_ASSERT_TRUE(c.enabled());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_riseOpensFallCloses);
    RUN_TEST(test_renewalEvery60s);
    RUN_TEST(test_glitchIgnored);
    RUN_TEST(test_disableDrainsActiveInputs);
    RUN_TEST(test_twoInputsIndependent);
    RUN_TEST(test_serializeEnabledFlag);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: RED**; **Step 3: Implementar** (por entrada: `{rawLast, rawSinceMs, stable, lastRenewMs, needClose}`; `update` prioriza: needClose (disable-drain) → bordas → renovações).

- [ ] **Step 4: GREEN** (6 testes). **Step 5: Commit** — `feat(irrigation): add 24vac mirror mode engine with renewal and bypass state`

---

### Task 6: `IrrigationGateway` + wiring no módulo

**Files:**
- Create: `src/modules/irrigation/IrrigationGateway.h` / `.cpp`
- Modify: `src/modules/irrigation/IrrigationModule.h` / `.cpp`
- Modify: `test/native-suite-count` (38 → 43)
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (Fase 4 concluída)

**Interfaces:**
- Consumes: Tasks 1–5; `getValidTime(RTCQualityDevice, true)` (`src/gps/RTC.h`); codecs/module da F1–F3.
- Produces: `class IrrigationGateway` agregando `ZoneTable zones; StationRegistry stations; ProgramScheduler scheduler; CommandTracker tracker; StationMonitor monitor; AlertCenter alerts; MirrorMode mirror;` — tudo público (é um agregado; o módulo acessa direto).

Decisões (documentar em comentários):

1. **Fonte de comandos**: `IrrigationGateway` NÃO envia rádio. O módulo tem `void gwSendValveCmd(uint32_t node, uint8_t index, uint8_t action, uint16_t durationS, uint8_t zoneId, uint8_t attempts)` que encoda `CmdValvula{index, action, durationS}` (ou `CmdGpo` quando `tipo==1`), envia, e chama `gateway.tracker.track(seq, ...)`.
2. **Loop do gateway** (`gwTick()`, chamado no `runOnce` quando role GATEWAY, 1×/s):
   - `scheduler.tick(getValidTime(RTCQualityDevice, true))` em loop até NONE; OPEN → resolve zona (`zones.byId`), clamp `min(durationS, maxMin*60)`, **suprime se `mirror.enabled() && zona.fonteInput >= 0 && mirror.inputActive(fonteInput)`** (bypass: espelho manda); CLOSE → envia fechar.
   - Espelho: lê GPIO das entradas (`settings.pinsDigitalIn[i]`, polaridade `digitalInActiveLow`), monta bitmap, `mirror.update(bitmap, millis())` em loop até NONE; OPEN → `zones.byFonte(input)` → envia abrir `OPEN_S` **sem clamp de maxMin** (bypass total; estação clampa só no teto compilado); CLOSE → fechar. Sem zona mapeada → ignora com LOG_DEBUG.
   - `tracker.poll(millis())` em loop: RESEND → reenvia com `++txSeq` e `retrack`; FAILED → `alerts.push({CMD_FAIL, node, zoneId})` + LOG_WARN.
   - Silêncio: para cada estação do registry, `monitor.checkSilence(node, silencioAlertaMin*60000, millis(), a)` → push.
3. **handleReceived (role GATEWAY)**:
   - `MSG_ACK`: decode; `tracker.onAck(mp.from, ack.ackedSeq)`; reconciliação de epoch com `ack.configEpoch` (mesma regra do HB).
   - `MSG_HEARTBEAT`: decode; `monitor.onHeartbeat(...)` → push alerts; reconciliação: entry = `stations.byNode(mp.from)`; se `hb.configEpoch < entry->desiredEpoch` e cooldown de 30 s ok → envia o blob (1 fragmento SET_CONFIG, epoch = desiredEpoch); se `hb.configEpoch > entry->desiredEpoch` → envia GET_CONFIG (cooldown 30 s).
   - `MSG_SET_CONFIG` **no role GATEWAY** = resposta de GET_CONFIG: reassembler → COMPLETE → `stations.adoptConfig(mp.from, blob, epoch)` + persistir + `alerts.push({CONFIG_ADOPTED, mp.from, epoch})`. (Estação continua com o fluxo da F2; o switch por role acontece no handler.)
   - `MSG_EVENTO`: LOG_INFO com code/arg (auditoria plena = Fase 6).
4. **Pareamento → registro**: no `handlePairAnnounce`, além da allowlist, `stations.upsert({node, name do announce, epoch 0, ...defaults})` + persistir.
5. **Persistência**: `loadGatewayState()/saveGatewayState()` no módulo — 4 arquivos staged (`irrigation-stations.dat`, `-zones.dat`, `-programs.dat`, `-mirror.dat`), buffers `uint8_t buf[6 + 16*87]` etc.; load no construtor (role GATEWAY), save após cada mutação (adoção, upsert por pareamento, toggle espelho).
6. **Zona → NACK crítico**: ACK com `status != ACK_OK` resolve a pendência igualmente (não retenta comando recusado — NACK é resposta definitiva; alerta `CMD_FAIL` com `arg = reason`).

Passos:

- [ ] **Step 1**: Criar `IrrigationGateway.{h,cpp}` (agregado trivial: membros públicos + `loadDefaults()` vazio; a lógica condicional vive no módulo — mantê-lo header-only se preferir, com .cpp vazio para o build).
- [ ] **Step 2**: Wiring no módulo conforme decisões 1–6 (handlers, gwTick, persistência, announce→registro). O `runOnce` do role GATEWAY passa a: `valves.tick` (já global) → `gatewayPairing.tick` → `gwTick()` → `refreshLedMode()` → retornar `1000` (1 s; a cadência de 1 s substitui o retorno anterior de 60 s/1 s condicional).
- [ ] **Step 3**: Compilar via suites componentes (`test_irrigation_gwtables`, `test_irrigation_mirror` — compilam src inteiro) até limpo.
- [ ] **Step 4**: `test/native-suite-count` 38 → 43; roadmap Fase 4 → `concluída (data, plano 2026-07-12-irrigacao-fase4-gateway.md; edição de zonas/programas/espelho via UI chega na Fase 5; intertravamentos na Fase 6; máquina guiada por ACK na Fase 7)`.
- [ ] **Step 5**: Rodar as 5 suites novas + `test_irrigation_valve` (regressão). **NÃO rodar a suite completa — o controller roda.**
- [ ] **Step 6**: Commits:

```bash
git add src/modules/irrigation/IrrigationGateway.* src/modules/irrigation/IrrigationModule.*
git commit -m "feat(irrigation): gateway engine - scheduler, retries, epoch reconciliation, alerts and mirror mode"
git add test/native-suite-count docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "test(irrigation): register phase 4 suites and update roadmap"
```

---

## Self-Review (executado na escrita do plano)

- **Cobertura**: §5.2 zonas ✔ (fonte = espelho); §5.3 registro (nome/nó substituível/params/coords lat-lon; `descricao` textual = Fase 5 UI) ✔; §5.4 reconciliação + regra do maior epoch ✔ (auditoria plena Fase 6); §7.1 programas ✔ (edição via UI = Fase 5); §8.1 níveis+histerese ✔; §8.2 reboots ✔ (causa detalhada do reboot = estação já reporta campo; análise fina Fase 6); §8.3 silêncio ✔ (tendência de SNR = Fase 6); retries+alerta §4.2 ✔; espelho do usuário ✔ (bypass total de config/cronograma; intertravamentos ainda não existem — quando chegarem na Fase 6, DEVEM respeitar o bypass registrado no roadmap).
- **Placeholders**: nenhum; Task 6 é integração com decisões numeradas e assinaturas fechadas (mesmo modelo das fases anteriores).
- **Consistência**: `Zone/ZoneTable/StationEntry/StationRegistry` idênticos Tasks 1/6; `SchedAction`/`tick(uint32_t)` Tasks 2/6; `CommandTracker::{track,onAck,retrack,poll}` Tasks 3/6; `AlertType/Alert/AlertCenter/StationMonitor` Tasks 4/6; `MirrorMode::{update,inputActive,setEnabled}` Tasks 5/6; limites globais (16/24/8/8/32) consistentes.
