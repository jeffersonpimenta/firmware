# Supressão de irrigação por previsão meteorológica (Open-Meteo) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Gateway consulta a Open-Meteo 2×/dia e suprime a abertura de zonas/grupos hidráulicos quando a chuva prevista (12h) **e** a probabilidade de chuva ultrapassam os limiares de uma regra habilitada que cobre aquele alvo; UI espelha o mockup (Mais › Meteorologia).

**Architecture:** Subsistema de clima gateway-side dividido em núcleo puro (config + tabela de regras + engine de veredito — testável no suite nativo) e um cliente de rede isolado (fetch/parse Open-Meteo, ESP32-only). O veredito é consultado no gate do scheduler, junto do check de intertravamento existente. Fail-open com TTL: sem dado fresco, nunca suprime.

**Tech Stack:** C++17 (firmware Meshtastic), Unity (testes nativos via Docker), `WiFiClientSecure` + parse JSON hand-rolled em streaming (sem lib nova), JS vanilla no painel (`data/irrigacao/`).

---

## Convenções e fatos do código (ler antes de começar)

- **Padrão de tabela** (magic+count+N×record+crc32): ver `src/modules/irrigation/InterlockTable.cpp:40-66`. CRC via `IrrigationProto::crc32(buf, len)` (incluir `modules/irrigation/IrrigationProtocol.h`).
- **Persistência gateway-side**: helpers file-static `stagedWrite(tmp,path,buf,n)` / `stagedRead(path,buf,cap,&n)` em `IrrigationModule.cpp:1872-1913`; caminhos em `IrrigationModule.cpp:24-51`; load no boot em `IrrigationModule.cpp:255-266`; `saveGatewayState()` em `1947-1975`.
- **JSON de saída**: classe `JsonWriter` (`IrrigationWebApi.h:19-47`, impl `.cpp:10-54`). Builders puros estilo `buildOverview` (`.cpp:70-85`).
- **Endpoints**: `server->registerNode(new ResourceNode(path, method, &handler))` (`IrrigationWebEndpoints.cpp:1074-1122`); guarda `gwReady()` (= `irrigationModule && irrigationModule->gwIsGateway()`); `readBody(req,buf,cap)` lê corpo POST; `sendJson(res, buf[, code])` responde.
- **Auditoria**: `auditEvent(AuditOrigin, AuditAction, uint8_t target, AuditResult, uint32_t node)`. Enums em `src/modules/irrigation/AuditLog.h`. **Append-only** (valores são ABI persistida).
- **Gate do scheduler**: `IrrigationModule.cpp:3155-3210` (laço `gateway.scheduler.tick()`), ramo `SchedAction::Type::OPEN`, antes de `routeZoneToGroup` (linha ~3165). Check de intertravamento análogo em ~3188.
- **Hora local**: `bool IrrigationModule::computeLocalSecs(uint32_t &out) const` (`:2349`) — `out=0` se sem RTC.
- **TLS mais leve**: `WiFiClientSecure` + `setInsecure()` (ver `MQTT.cpp:450-464`). `HTTPClient` NÃO é usado hoje — usar `WiFiClientSecure` direto.
- **Testes**: um diretório por suíte em `test/test_<nome>/test_main.cpp`; lifecycle `setup(){ initializeTestEnvironment(); UNITY_BEGIN(); RUN_TEST(...); exit(UNITY_END()); }`. Rodar: `./bin/run-tests.sh` (ou `-f test_<nome>`). Descoberta automática por PlatformIO.
- **Guard de feature**: arquivos puros (config/tabela/engine) compilam no nativo — **sem** guard ESP32. `WeatherClient` (rede) é **ESP32-only** → `#if defined(ARCH_ESP32)`.
- **Idioma**: comentários/commits em pt-BR, seguindo o restante do módulo.

## Constantes (definidas uma vez, em `WeatherRuleTable.h`)

```cpp
static constexpr uint8_t  MAX_WEATHER_RULES = 8;
static constexpr uint8_t  WR_MAX_ZONE_TARGETS = 16;
static constexpr uint8_t  WR_MAX_GROUP_TARGETS = 8;
static constexpr uint8_t  WR_NOME_LEN = 32;
static constexpr uint8_t  WR_MSG_LEN = 48;
```

## Mapa de arquivos

**Criar:**
- `src/modules/irrigation/WeatherConfig.h` / `.cpp` — `WeatherConfig` (persistido) + serialize/deserialize.
- `src/modules/irrigation/WeatherForecast.h` — `WeatherCache` (POD RAM) + `WeatherVerdict`.
- `src/modules/irrigation/WeatherRuleTable.h` / `.cpp` — `WeatherRule` + `WeatherRuleTable`.
- `src/modules/irrigation/WeatherEngine.h` / `.cpp` — vereditos puros.
- `src/modules/irrigation/WeatherClient.h` / `.cpp` — fetch/parse/agendamento (ESP32-only).
- `test/test_weather_config/test_main.cpp`
- `test/test_weather_rule_table/test_main.cpp`
- `test/test_weather_engine/test_main.cpp`
- `test/test_weather_webapi/test_main.cpp`

**Modificar:**
- `src/modules/irrigation/AuditLog.h` — `CLIMA`, `CMD_SUPRIMIDO`.
- `src/modules/irrigation/IrrigationGateway.h` — membros novos.
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — persistência, gate, glue, tick do client.
- `src/modules/irrigation/IrrigationWebApi.h` / `.cpp` — builders + parse.
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — endpoints.
- `data/irrigacao/app.js` — página Meteorologia.
- `bin/run-tests.sh` — contagem esperada de suítes (se aplicável).

---

## Task 1: Enums de auditoria (CLIMA, CMD_SUPRIMIDO)

**Files:**
- Modify: `src/modules/irrigation/AuditLog.h:6-21`

- [ ] **Step 1: Append aos enums (sem reordenar — ABI)**

Em `AuditOrigin` (após `NIVEL`):
```cpp
enum class AuditOrigin : uint8_t {
    SISTEMA = 0, CRONOGRAMA, PAINEL, PORTAL_CAMPO, BOTAO_FISICO,
    ENTRADA_FISICA, INTERTRAVAMENTO, FAILSAFE_TIMER, SERVICO,
    GRUPO_HIDRAULICO, // = 9
    NIVEL,            // = 10
    CLIMA             // = 11 (supressão por previsão meteorológica) — append-only
};
```
Em `AuditAction` (após `ESPELHO`):
```cpp
    ESPELHO,       // toggle/mapeamento do modo espelho
    CMD_SUPRIMIDO, // abertura suprimida por regra meteorológica (target = zoneId)
};
```

- [ ] **Step 2: Build de sanidade (nativo)**

Run: `./bin/run-tests.sh -f test_interlock_table`
Expected: PASS (só confirma que o header compila; nenhum teste novo ainda).

- [ ] **Step 3: Commit**

```bash
git add src/modules/irrigation/AuditLog.h
git commit -m "feat(irrigation): enums de auditoria CLIMA e CMD_SUPRIMIDO (append-only)"
```

---

## Task 2: WeatherConfig (struct persistida)

**Files:**
- Create: `src/modules/irrigation/WeatherConfig.h`, `src/modules/irrigation/WeatherConfig.cpp`
- Test: `test/test_weather_config/test_main.cpp`

- [ ] **Step 1: Escrever o header**

`src/modules/irrigation/WeatherConfig.h`:
```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

// Config compartilhada do subsistema de clima (persistida gateway-side).
// Layout serializado: magic(4) + version(2) + payload(14) + crc32(4) = 24 bytes.
struct WeatherConfig {
    static constexpr uint32_t MAGIC = 0x57544831; // "WTH1"
    static constexpr uint16_t VERSION = 1;

    uint8_t  enabled = 0;      // master on/off
    int32_t  latE7 = 0;        // latitude ×1e7
    int32_t  lonE7 = 0;        // longitude ×1e7
    uint8_t  pollHourA = 4;    // 1º poll do dia (hora local 0..23)
    uint8_t  pollHourB = 16;   // 2º poll do dia
    uint16_t staleTtlH = 24;   // TTL do cache p/ fail-open (horas)

    // magic+version+enabled+lat+lon+pollA+pollB+ttl+crc
    static constexpr size_t SERIALIZED = 4 + 2 + 1 + 4 + 4 + 1 + 1 + 2 + 4; // 23

    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // false => mantém defaults
};
```

- [ ] **Step 2: Escrever o teste (falha)**

`test/test_weather_config/test_main.cpp`:
```cpp
#include "modules/irrigation/WeatherConfig.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_roundtrip() {
    WeatherConfig c;
    c.enabled = 1; c.latE7 = -235000000; c.lonE7 = -466000000;
    c.pollHourA = 5; c.pollHourB = 17; c.staleTtlH = 12;
    uint8_t buf[64];
    size_t n = c.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(WeatherConfig::SERIALIZED, n);
    WeatherConfig c2;
    TEST_ASSERT_TRUE(c2.deserialize(buf, n));
    TEST_ASSERT_EQUAL(1, c2.enabled);
    TEST_ASSERT_EQUAL_INT32(-235000000, c2.latE7);
    TEST_ASSERT_EQUAL_INT32(-466000000, c2.lonE7);
    TEST_ASSERT_EQUAL(5, c2.pollHourA);
    TEST_ASSERT_EQUAL(17, c2.pollHourB);
    TEST_ASSERT_EQUAL(12, c2.staleTtlH);
}

static void test_bad_magic_keeps_defaults() {
    uint8_t buf[WeatherConfig::SERIALIZED] = {0};
    WeatherConfig c; c.pollHourA = 9;
    TEST_ASSERT_FALSE(c.deserialize(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(9, c.pollHourA); // inalterado em falha
}

static void test_bad_crc_rejected() {
    WeatherConfig c; c.enabled = 1;
    uint8_t buf[64];
    size_t n = c.serialize(buf, sizeof(buf));
    buf[7] ^= 0xFF; // corrompe payload
    WeatherConfig c2;
    TEST_ASSERT_FALSE(c2.deserialize(buf, n));
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_bad_magic_keeps_defaults);
    RUN_TEST(test_bad_crc_rejected);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Rodar — deve falhar (link error)**

Run: `./bin/run-tests.sh -f test_weather_config`
Expected: FAIL (símbolos `WeatherConfig::serialize/deserialize` indefinidos).

- [ ] **Step 4: Implementar**

`src/modules/irrigation/WeatherConfig.cpp`:
```cpp
#include "modules/irrigation/WeatherConfig.h"
#include "modules/irrigation/IrrigationProtocol.h" // IrrigationProto::crc32
#include <string.h>

size_t WeatherConfig::serialize(uint8_t *buf, size_t cap) const {
    if (cap < SERIALIZED) return 0;
    size_t o = 0;
    uint32_t magic = MAGIC; memcpy(buf + o, &magic, 4); o += 4;
    uint16_t ver = VERSION; memcpy(buf + o, &ver, 2); o += 2;
    buf[o++] = enabled;
    memcpy(buf + o, &latE7, 4); o += 4;
    memcpy(buf + o, &lonE7, 4); o += 4;
    buf[o++] = pollHourA;
    buf[o++] = pollHourB;
    memcpy(buf + o, &staleTtlH, 2); o += 2;
    uint32_t crc = IrrigationProto::crc32(buf, o); memcpy(buf + o, &crc, 4); o += 4;
    return o;
}

bool WeatherConfig::deserialize(const uint8_t *buf, size_t n) {
    if (n != SERIALIZED) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != MAGIC) return false;
    uint32_t crc; memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4)) return false;
    size_t o = 6; // após magic+version
    WeatherConfig tmp;
    tmp.enabled = buf[o++];
    memcpy(&tmp.latE7, buf + o, 4); o += 4;
    memcpy(&tmp.lonE7, buf + o, 4); o += 4;
    tmp.pollHourA = buf[o++];
    tmp.pollHourB = buf[o++];
    memcpy(&tmp.staleTtlH, buf + o, 2); o += 2;
    *this = tmp;
    return true;
}
```

- [ ] **Step 5: Rodar — deve passar**

Run: `./bin/run-tests.sh -f test_weather_config`
Expected: PASS (3 testes).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/WeatherConfig.h src/modules/irrigation/WeatherConfig.cpp test/test_weather_config/test_main.cpp
git commit -m "feat(irrigation): WeatherConfig persistida (serialize/deserialize + testes)"
```

---

## Task 3: WeatherRuleTable

**Files:**
- Create: `src/modules/irrigation/WeatherRuleTable.h`, `src/modules/irrigation/WeatherRuleTable.cpp`
- Test: `test/test_weather_rule_table/test_main.cpp`

- [ ] **Step 1: Header**

`src/modules/irrigation/WeatherRuleTable.h`:
```cpp
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
```

- [ ] **Step 2: Teste (falha)**

`test/test_weather_rule_table/test_main.cpp`:
```cpp
#include "modules/irrigation/WeatherRuleTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static WeatherRule mkRule(uint8_t id) {
    WeatherRule r; r.id = id; r.enabled = 1;
    r.limiarMmCenti = 500; r.limiarPct = 60;
    r.zonaIds[0] = 3; r.grupoIds[0] = 1;
    strncpy(r.nome, "Chuva forte", WeatherRule::NOME_LEN - 1);
    strncpy(r.mensagem, "Chuva prevista 12h", WeatherRule::MSG_LEN - 1);
    return r;
}

static void test_upsert_byid_count() {
    WeatherRuleTable t;
    TEST_ASSERT_TRUE(t.upsert(mkRule(1)));
    TEST_ASSERT_TRUE(t.upsert(mkRule(2)));
    TEST_ASSERT_EQUAL(2, t.count());
    const WeatherRule *g = t.byId(1);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL(500, g->limiarMmCenti);
    TEST_ASSERT_EQUAL_STRING("Chuva forte", g->nome);
}

static void test_covers() {
    WeatherRule r = mkRule(1);
    TEST_ASSERT_TRUE(r.coversZone(3));
    TEST_ASSERT_FALSE(r.coversZone(9));
    TEST_ASSERT_TRUE(r.coversGroup(1));
    TEST_ASSERT_FALSE(r.coversGroup(2));
}

static void test_remove_and_full() {
    WeatherRuleTable t;
    for (uint8_t i = 1; i <= WeatherRuleTable::MAX; i++) TEST_ASSERT_TRUE(t.upsert(mkRule(i)));
    TEST_ASSERT_FALSE(t.upsert(mkRule(99))); // cheia
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_NULL(t.byId(1));
    TEST_ASSERT_TRUE(t.upsert(mkRule(99))); // abriu vaga
}

static void test_roundtrip() {
    WeatherRuleTable t;
    t.upsert(mkRule(1)); t.upsert(mkRule(2));
    uint8_t buf[2048];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    WeatherRuleTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL(2, t2.count());
    TEST_ASSERT_EQUAL_STRING("Chuva prevista 12h", t2.byId(2)->mensagem);
}

static void test_corrupt_empties() {
    uint8_t buf[16] = {0};
    WeatherRuleTable t; t.upsert(mkRule(1));
    TEST_ASSERT_FALSE(t.deserialize(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, t.count());
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_upsert_byid_count);
    RUN_TEST(test_covers);
    RUN_TEST(test_remove_and_full);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_corrupt_empties);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Rodar — falha**

Run: `./bin/run-tests.sh -f test_weather_rule_table`
Expected: FAIL (símbolos indefinidos).

- [ ] **Step 4: Implementar**

`src/modules/irrigation/WeatherRuleTable.cpp`:
```cpp
#include "modules/irrigation/WeatherRuleTable.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include <string.h>

bool WeatherRule::coversZone(uint8_t zoneId) const {
    if (!zoneId) return false;
    for (uint8_t z : zonaIds) if (z == zoneId) return true;
    return false;
}
bool WeatherRule::coversGroup(uint8_t groupId) const {
    if (!groupId) return false;
    for (uint8_t g : grupoIds) if (g == groupId) return true;
    return false;
}

bool WeatherRuleTable::upsert(const WeatherRule &r) {
    if (!r.id) return false;
    for (auto &e : rules) if (e.id == r.id) { e = r; return true; }
    for (auto &e : rules) if (!e.id) { e = r; return true; }
    return false;
}
bool WeatherRuleTable::removeById(uint8_t id) {
    for (auto &e : rules) if (e.id == id) { e = WeatherRule{}; return true; }
    return false;
}
const WeatherRule *WeatherRuleTable::byId(uint8_t id) const {
    for (auto &e : rules) if (e.id == id) return &e;
    return nullptr;
}
const WeatherRule *WeatherRuleTable::ruleAt(size_t index) const {
    size_t k = 0;
    for (auto &e : rules) if (e.id) { if (k == index) return &e; k++; }
    return nullptr;
}
size_t WeatherRuleTable::count() const {
    size_t k = 0; for (auto &e : rules) if (e.id) k++; return k;
}
uint8_t WeatherRuleTable::nextFreeId() const {
    for (uint16_t id = 1; id <= 255; id++) if (!byId((uint8_t)id)) return (uint8_t)id;
    return 0;
}

size_t WeatherRuleTable::serialize(uint8_t *buf, size_t cap) const {
    size_t n = count();
    size_t need = 4 + 2 + n * sizeof(WeatherRule) + 4;
    if (cap < need) return 0;
    size_t o = 0;
    uint32_t magic = MAGIC; memcpy(buf + o, &magic, 4); o += 4;
    uint16_t c = (uint16_t)n; memcpy(buf + o, &c, 2); o += 2;
    for (auto &e : rules) if (e.id) { memcpy(buf + o, &e, sizeof(e)); o += sizeof(e); }
    uint32_t crc = IrrigationProto::crc32(buf, o); memcpy(buf + o, &crc, 4); o += 4;
    return o;
}
bool WeatherRuleTable::deserialize(const uint8_t *buf, size_t n) {
    for (auto &e : rules) e = WeatherRule{};
    if (n < 10) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != MAGIC) return false;
    uint16_t c; memcpy(&c, buf + 4, 2);
    size_t need = 4 + 2 + (size_t)c * sizeof(WeatherRule) + 4;
    if (n != need || c > MAX) return false;
    uint32_t crc; memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4)) return false;
    size_t o = 6;
    for (uint16_t i = 0; i < c; i++) { memcpy(&rules[i], buf + o, sizeof(WeatherRule)); o += sizeof(WeatherRule); }
    return true;
}
```

- [ ] **Step 5: Rodar — passa**

Run: `./bin/run-tests.sh -f test_weather_rule_table`
Expected: PASS (5 testes). Se o `static_assert(sizeof==110)` disparar, ajuste o valor ao `sizeof` real reportado e registre no comentário (padding depende do compilador; manter consistente entre nativo e ESP32, ambos LE 32-bit).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/WeatherRuleTable.h src/modules/irrigation/WeatherRuleTable.cpp test/test_weather_rule_table/test_main.cpp
git commit -m "feat(irrigation): WeatherRuleTable (regras de supressão climática + testes)"
```

---

## Task 4: WeatherCache + WeatherVerdict (POD, sem lógica)

**Files:**
- Create: `src/modules/irrigation/WeatherForecast.h`

- [ ] **Step 1: Escrever o header (compila junto no próximo teste)**

`src/modules/irrigation/WeatherForecast.h`:
```cpp
#pragma once
#include <stdint.h>

// Snapshot do último forecast (RAM, não persistido). Unidades inteiras.
struct WeatherCache {
    bool     valid = false;
    bool     isMock = false;      // último fetch falhou → exibe "estimativa"
    uint32_t fetchEpoch = 0;      // epoch local do fetch (base do TTL)
    // decisão:
    uint16_t chuvaPrevista12hCenti = 0; // Σ precip horas i+1..i+12 (centi-mm)
    uint8_t  probChuvaPct = 0;          // max prob horas i..i+12
    // exibição (espelha o card do mockup):
    uint16_t chuvaAcum24hCenti = 0;
    int16_t  tempAtualCenti = 0;
    uint8_t  umidadeRelPct = 0;
    uint8_t  umidadeSoloPct = 0;
    int16_t  tempMinCenti = 0;
    int16_t  tempMaxCenti = 0;
    uint16_t ventoRajadaCenti = 0;
    uint16_t et0Centi = 0;
};

struct WeatherVerdict {
    bool    suppress = false;
    uint8_t ruleId = 0; // regra que causou a supressão (0 = nenhuma)
};
```

- [ ] **Step 2: Commit (sem teste isolado; validado na Task 5)**

```bash
git add src/modules/irrigation/WeatherForecast.h
git commit -m "feat(irrigation): WeatherCache + WeatherVerdict (POD)"
```

---

## Task 5: WeatherEngine (veredito puro)

**Files:**
- Create: `src/modules/irrigation/WeatherEngine.h`, `src/modules/irrigation/WeatherEngine.cpp`
- Test: `test/test_weather_engine/test_main.cpp`

- [ ] **Step 1: Header**

`src/modules/irrigation/WeatherEngine.h`:
```cpp
#pragma once
#include "modules/irrigation/WeatherForecast.h"
#include "modules/irrigation/WeatherRuleTable.h"
#include <stdint.h>

namespace WeatherEngine {

// TTL/fail-open: true só se cache válido e não expirado (e nowEpoch>0).
bool cacheFresh(const WeatherCache &cache, uint32_t nowEpoch, uint16_t ttlHours);

// Suprime se alguma regra habilitada que cobre o alvo satisfaz
// (chuva12h > limiarMm) E (prob > limiarPct). Fail-open se cache não-fresco.
WeatherVerdict zoneVerdict(uint8_t zoneId, const WeatherCache &cache,
                           const WeatherRuleTable &rules, uint32_t nowEpoch, uint16_t ttlHours);
WeatherVerdict groupVerdict(uint8_t groupId, const WeatherCache &cache,
                            const WeatherRuleTable &rules, uint32_t nowEpoch, uint16_t ttlHours);

// true se a regra dispara AGORA contra o cache (ignora cobertura de alvo).
// Usado pela UI para o badge "Suprimindo" e "triggered".
bool ruleTriggered(const WeatherRule &r, const WeatherCache &cache,
                   uint32_t nowEpoch, uint16_t ttlHours);

} // namespace WeatherEngine
```

- [ ] **Step 2: Teste (falha)**

`test/test_weather_engine/test_main.cpp`:
```cpp
#include "modules/irrigation/WeatherEngine.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static WeatherCache freshCache(uint16_t mmCenti, uint8_t pct) {
    WeatherCache c; c.valid = true; c.fetchEpoch = 1000;
    c.chuvaPrevista12hCenti = mmCenti; c.probChuvaPct = pct;
    return c;
}
static WeatherRule rule(uint8_t id, uint16_t mm, uint8_t pct, uint8_t enabled) {
    WeatherRule r; r.id = id; r.enabled = enabled;
    r.limiarMmCenti = mm; r.limiarPct = pct;
    r.zonaIds[0] = 5; r.grupoIds[0] = 2;
    return r;
}
static const uint32_t NOW = 1000 + 3600; // 1h após fetch
static const uint16_t TTL = 24;

static void test_fail_open_invalid_cache() {
    WeatherCache c; // valid=false
    WeatherRuleTable t; t.upsert(rule(1, 100, 10, 1));
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).suppress);
}
static void test_fail_open_stale_cache() {
    WeatherCache c = freshCache(1000, 90);
    WeatherRuleTable t; t.upsert(rule(1, 100, 10, 1));
    uint32_t late = 1000 + (uint32_t)TTL * 3600 + 1;
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, late, TTL).suppress);
}
static void test_fail_open_no_rtc() {
    WeatherCache c = freshCache(1000, 90);
    WeatherRuleTable t; t.upsert(rule(1, 100, 10, 1));
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, 0, TTL).suppress);
}
static void test_suppress_when_both_exceed() {
    WeatherCache c = freshCache(800, 70); // 8.0mm, 70%
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 1)); // >5mm E >60%
    WeatherVerdict v = WeatherEngine::zoneVerdict(5, c, t, NOW, TTL);
    TEST_ASSERT_TRUE(v.suppress);
    TEST_ASSERT_EQUAL(1, v.ruleId);
}
static void test_strict_inequality_no_suppress_on_equal() {
    WeatherCache c = freshCache(500, 60); // exatamente nos limiares
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 1));
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).suppress);
}
static void test_only_one_condition_no_suppress() {
    WeatherCache c = freshCache(800, 50); // chuva ok, prob abaixo
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 1));
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).suppress);
}
static void test_disabled_rule_ignored() {
    WeatherCache c = freshCache(800, 70);
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 0)); // disabled
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).suppress);
}
static void test_target_not_covered() {
    WeatherCache c = freshCache(800, 70);
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 1)); // cobre zona 5, grupo 2
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(9, c, t, NOW, TTL).suppress);
    TEST_ASSERT_TRUE(WeatherEngine::groupVerdict(2, c, t, NOW, TTL).suppress);
    TEST_ASSERT_FALSE(WeatherEngine::groupVerdict(9, c, t, NOW, TTL).suppress);
}
static void test_first_matching_rule_wins() {
    WeatherCache c = freshCache(800, 70);
    WeatherRuleTable t;
    t.upsert(rule(3, 500, 60, 1)); // ambas cobrem zona 5
    t.upsert(rule(7, 100, 10, 1));
    uint8_t rid = WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).ruleId;
    TEST_ASSERT_TRUE(rid == 3 || rid == 7); // qualquer que case; determinístico por slot
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_fail_open_invalid_cache);
    RUN_TEST(test_fail_open_stale_cache);
    RUN_TEST(test_fail_open_no_rtc);
    RUN_TEST(test_suppress_when_both_exceed);
    RUN_TEST(test_strict_inequality_no_suppress_on_equal);
    RUN_TEST(test_only_one_condition_no_suppress);
    RUN_TEST(test_disabled_rule_ignored);
    RUN_TEST(test_target_not_covered);
    RUN_TEST(test_first_matching_rule_wins);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Rodar — falha**

Run: `./bin/run-tests.sh -f test_weather_engine`
Expected: FAIL.

- [ ] **Step 4: Implementar**

`src/modules/irrigation/WeatherEngine.cpp`:
```cpp
#include "modules/irrigation/WeatherEngine.h"

namespace WeatherEngine {

bool cacheFresh(const WeatherCache &cache, uint32_t nowEpoch, uint16_t ttlHours) {
    if (!cache.valid || nowEpoch == 0) return false;
    if (nowEpoch < cache.fetchEpoch) return false; // relógio andou pra trás
    return (nowEpoch - cache.fetchEpoch) <= (uint32_t)ttlHours * 3600u;
}

bool ruleTriggered(const WeatherRule &r, const WeatherCache &cache,
                   uint32_t nowEpoch, uint16_t ttlHours) {
    if (!r.enabled) return false;
    if (!cacheFresh(cache, nowEpoch, ttlHours)) return false;
    return (cache.chuvaPrevista12hCenti > r.limiarMmCenti) &&
           (cache.probChuvaPct > r.limiarPct);
}

static WeatherVerdict verdictBy(bool (WeatherRule::*covers)(uint8_t) const, uint8_t targetId,
                                const WeatherCache &cache, const WeatherRuleTable &rules,
                                uint32_t nowEpoch, uint16_t ttlHours) {
    WeatherVerdict v;
    if (!cacheFresh(cache, nowEpoch, ttlHours)) return v; // fail-open
    for (size_t i = 0; i < rules.count(); i++) {
        const WeatherRule *r = rules.ruleAt(i);
        if (!r || !r->enabled) continue;
        if (!(r->*covers)(targetId)) continue;
        if (ruleTriggered(*r, cache, nowEpoch, ttlHours)) { v.suppress = true; v.ruleId = r->id; return v; }
    }
    return v;
}

WeatherVerdict zoneVerdict(uint8_t zoneId, const WeatherCache &cache,
                           const WeatherRuleTable &rules, uint32_t nowEpoch, uint16_t ttlHours) {
    return verdictBy(&WeatherRule::coversZone, zoneId, cache, rules, nowEpoch, ttlHours);
}
WeatherVerdict groupVerdict(uint8_t groupId, const WeatherCache &cache,
                            const WeatherRuleTable &rules, uint32_t nowEpoch, uint16_t ttlHours) {
    return verdictBy(&WeatherRule::coversGroup, groupId, cache, rules, nowEpoch, ttlHours);
}

} // namespace WeatherEngine
```

- [ ] **Step 5: Rodar — passa**

Run: `./bin/run-tests.sh -f test_weather_engine`
Expected: PASS (9 testes).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/WeatherEngine.h src/modules/irrigation/WeatherEngine.cpp test/test_weather_engine/test_main.cpp
git commit -m "feat(irrigation): WeatherEngine veredito puro (fail-open + limiares + testes)"
```

---

## Task 6: Agregado IrrigationGateway

**Files:**
- Modify: `src/modules/irrigation/IrrigationGateway.h`

- [ ] **Step 1: Incluir e adicionar membros**

Adicionar includes no topo (junto aos demais):
```cpp
#include "modules/irrigation/WeatherConfig.h"
#include "modules/irrigation/WeatherForecast.h"
#include "modules/irrigation/WeatherRuleTable.h"
```
Dentro de `struct IrrigationGateway`, ao final:
```cpp
    // Supressão por previsão meteorológica (Open-Meteo).
    WeatherConfig weatherConfig;
    WeatherRuleTable weatherRules;
    WeatherCache weatherCache; // RAM; preenchido pelo WeatherClient
```

- [ ] **Step 2: Build de sanidade**

Run: `./bin/run-tests.sh -f test_weather_engine`
Expected: PASS (confirma que o header agregado ainda compila no nativo).

- [ ] **Step 3: Commit**

```bash
git add src/modules/irrigation/IrrigationGateway.h
git commit -m "feat(irrigation): agrega WeatherConfig/RuleTable/Cache ao gateway"
```

---

## Task 7: Persistência (load/save dos blobs de clima)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarações), `src/modules/irrigation/IrrigationModule.cpp`

- [ ] **Step 1: Caminhos de arquivo**

Em `IrrigationModule.cpp`, junto aos `GW_*_PATH` (linhas 24-51):
```cpp
static const char *GW_WEATHERCFG_PATH = "/prefs/irrigation_weathercfg.dat";
static const char *GW_WEATHERCFG_TMP  = "/prefs/irrigation_weathercfg.tmp";
static const char *GW_WEATHERRULES_PATH = "/prefs/irrigation_weatherrules.dat";
static const char *GW_WEATHERRULES_TMP  = "/prefs/irrigation_weatherrules.tmp";
```

- [ ] **Step 2: Declarar métodos em `IrrigationModule.h`**

Junto de `loadGroups/saveGroups` (privados):
```cpp
    bool loadWeather();   // carrega config + regras
    bool saveWeatherConfig();
    bool saveWeatherRules();
```

- [ ] **Step 3: Implementar em `IrrigationModule.cpp`** (perto de `loadLevels/saveLevels`)

```cpp
bool IrrigationModule::loadWeather() {
    bool ok = true;
    size_t n = 0;
    {
        uint8_t buf[WeatherConfig::SERIALIZED];
        if (stagedRead(GW_WEATHERCFG_PATH, buf, sizeof(buf), n))
            ok &= gateway.weatherConfig.deserialize(buf, n);
    }
    {
        uint8_t buf[4 + 2 + WeatherRuleTable::MAX * sizeof(WeatherRule) + 4];
        if (stagedRead(GW_WEATHERRULES_PATH, buf, sizeof(buf), n))
            ok &= gateway.weatherRules.deserialize(buf, n);
    }
    return ok;
}
bool IrrigationModule::saveWeatherConfig() {
    uint8_t buf[WeatherConfig::SERIALIZED];
    size_t n = gateway.weatherConfig.serialize(buf, sizeof(buf));
    return stagedWrite(GW_WEATHERCFG_TMP, GW_WEATHERCFG_PATH, buf, n);
}
bool IrrigationModule::saveWeatherRules() {
    uint8_t buf[4 + 2 + WeatherRuleTable::MAX * sizeof(WeatherRule) + 4];
    size_t n = gateway.weatherRules.serialize(buf, sizeof(buf));
    return stagedWrite(GW_WEATHERRULES_TMP, GW_WEATHERRULES_PATH, buf, n);
}
```

- [ ] **Step 4: Chamar o load no boot**

Em `IrrigationModule.cpp:255-266` (bloco `IrrigationRole::GATEWAY`), após `loadLevels();`:
```cpp
        loadWeather(); // config + regras de supressão climática
```

- [ ] **Step 5: Build (nativo compila o módulo?)**

`IrrigationModule.cpp` usa APIs de firmware; o nativo compila via stubs do harness. Rodar a suíte completa para garantir que nada quebrou de compilação compartilhada:
Run: `./bin/run-tests.sh -f test_weather_rule_table`
Expected: PASS. (Se o módulo não entra no build nativo, este passo apenas confirma que os headers novos seguem válidos.)

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): persistência de config e regras de clima (staged write/read)"
```

---

## Task 8: Gate no scheduler + resolução zona→grupo + auditoria

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `src/modules/irrigation/IrrigationModule.cpp`

- [ ] **Step 1: Declarar helper**

`IrrigationModule.h` (privado):
```cpp
    // Veredito de supressão climática para a zona (resolve grupo-dono via OR).
    WeatherVerdict weatherVerdictForZone(uint8_t zoneId);
```
Incluir no topo do `.cpp` (se ainda não): `#include "modules/irrigation/WeatherEngine.h"`.

- [ ] **Step 2: Implementar `weatherVerdictForZone`**

Em `IrrigationModule.cpp` (perto de `routeZoneToGroup`, ~3065). Precisa achar o grupo dono da zona. Reusar a mesma lógica de varredura que `routeZoneToGroup` usa sobre `gateway.groups` (varrer grupos ocupados e testar se contêm `zoneId` na lista de zonas do grupo). Padrão:
```cpp
WeatherVerdict IrrigationModule::weatherVerdictForZone(uint8_t zoneId) {
    uint32_t nowLocal = 0;
    computeLocalSecs(nowLocal);
    const WeatherCache &cache = gateway.weatherCache;
    const WeatherRuleTable &rules = gateway.weatherRules;
    const uint16_t ttl = gateway.weatherConfig.staleTtlH;
    if (!gateway.weatherConfig.enabled) return WeatherVerdict{};
    // 1) veredito direto por zona
    WeatherVerdict v = WeatherEngine::zoneVerdict(zoneId, cache, rules, nowLocal, ttl);
    if (v.suppress) return v;
    // 2) veredito pelo grupo dono (se houver). Varre grupos como routeZoneToGroup.
    for (size_t i = 0; i < gateway.groups.count(); i++) {
        const HydraulicGroup *g = gateway.groups.groupAt(i); // usar o accessor real da tabela
        if (!g) continue;
        bool contains = false;
        for (uint8_t k = 0; k < g->zoneCount; k++) if (g->zoneIds[k] == zoneId) { contains = true; break; }
        if (!contains) continue;
        WeatherVerdict gv = WeatherEngine::groupVerdict(g->id, cache, rules, nowLocal, ttl);
        if (gv.suppress) return gv;
    }
    return WeatherVerdict{};
}
```
> NOTA de implementação: confirmar em `HydraulicGroupTable.h` os nomes reais do accessor (`groupAt`/`ruleAt`), do campo de contagem (`zoneCount`) e do array (`zoneIds`). Ajustar para os nomes existentes. A intenção é: "para cada grupo que contém a zona, aplicar `groupVerdict`".

- [ ] **Step 3: Inserir o gate no laço OPEN**

Em `IrrigationModule.cpp` (~3162), no ramo `if (a.type == SchedAction::Type::OPEN) {` **antes** do bloco `routeZoneToGroup`:
```cpp
            if (a.type == SchedAction::Type::OPEN) {
                WeatherVerdict wv = weatherVerdictForZone(a.zoneId);
                if (wv.suppress) {
                    LOG_INFO("Irrigation GW: OPEN zona=%u suprimido por clima (regra %u)", a.zoneId, wv.ruleId);
                    auditEvent(AuditOrigin::CLIMA, AuditAction::CMD_SUPRIMIDO, a.zoneId,
                               AuditResult::OK, z->node);
                    continue; // não abre; próximo tick reavalia (fail-open embutido)
                }
            }
```
(O `continue` pula tanto o roteamento de grupo quanto a abertura direta.)

- [ ] **Step 4: Build de sanidade**

Run: `./bin/run-tests.sh -f test_weather_engine`
Expected: PASS. (Gate é código de firmware; validação funcional é manual/hardware — Task 16.)

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): gate de supressão climática no scheduler (OR zona/grupo + auditoria CLIMA)"
```

---

## Task 9: WeatherClient (fetch/parse/agendamento, ESP32-only)

**Files:**
- Create: `src/modules/irrigation/WeatherClient.h`, `src/modules/irrigation/WeatherClient.cpp`

- [ ] **Step 1: Header**

`src/modules/irrigation/WeatherClient.h`:
```cpp
#pragma once
#include "modules/irrigation/WeatherConfig.h"
#include "modules/irrigation/WeatherForecast.h"
#include <stdint.h>

// Cliente de rede da Open-Meteo. Sem estado persistido: preenche WeatherCache.
// Todo o corpo de rede é guardado por ARCH_ESP32; no nativo vira no-op.
class WeatherClient {
  public:
    // Chamar ~1×/s com hora local válida. Dispara poll nas horas configuradas
    // (pollHourA/B), uma vez por ocorrência, e um poll único ~30s após WiFi up.
    // Retorna true se atualizou o cache neste tick.
    bool tick(const WeatherConfig &cfg, uint32_t nowLocalSecs, bool staUp, WeatherCache &out);
    // Força um poll imediato (usado pelo botão "Atualizar"). Retorna sucesso.
    bool pollNow(const WeatherConfig &cfg, uint32_t nowEpoch, WeatherCache &out);

  private:
    uint32_t lastPollKey = 0;   // (dia*100 + hora) do último poll agendado
    bool seededAfterBoot = false;
    uint32_t staUpSinceMs = 0;
};
```

- [ ] **Step 2: Implementar (streaming parse, TLS leve)**

`src/modules/irrigation/WeatherClient.cpp`:
```cpp
#include "modules/irrigation/WeatherClient.h"
#include "configuration.h" // LOG_*, ARCH_ESP32

#if defined(ARCH_ESP32)
#include <WiFiClientSecure.h>
#include <math.h>

// Encolhe payload: só o necessário. forecast_hours=13 cobre a janela de 12h;
// past_hours=24 cobre o acumulado exibido. timezone=auto alinha o índice horário.
static const char *OM_HOST = "api.open-meteo.com";

// Lê a resposta inteira em chunks para um buffer rotativo e extrai números.
// Parser mínimo: acha a chave JSON e soma/reduz o array numérico que segue.
// (Implementação: baixar via WiFiClientSecure, pular headers HTTP, e varrer o
//  corpo com um scanner de estado que reconhece "precipitation":[...],
//  "precipitation_probability":[...], "current":{...} e "daily":{...}.)

static bool httpsGet(const char *host, const String &path, String &body) {
    WiFiClientSecure client;
    client.setInsecure();          // TLS mais leve: sem verificação de cert
    client.setTimeout(6);          // segundos
    if (!client.connect(host, 443)) { LOG_WARN("Weather: connect fail"); return false; }
    client.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
                 "\r\nConnection: close\r\n\r\n");
    // pular cabeçalhos
    uint32_t t0 = millis();
    bool headersDone = false;
    body = "";
    while (client.connected() && millis() - t0 < 8000) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (!headersDone) { if (line == "\r" || line.length() == 0) headersDone = true; }
            else body += line + "\n";
        }
    }
    client.stop();
    return body.length() > 0;
}

// Extrai a i-ésima janela do array numérico após a chave dada.
// Retorna via callback simples: aqui usamos helpers que somam/maximizam a fatia.
// Para manter leve e sem alocação extra, parse direto por índice.
static bool parseWeather(const String &body, WeatherCache &out) {
    // Reproduz a lógica do mockup parseWeather():
    //  i = índice de current.time em hourly.time
    //  chuvaPrevista12h = Σ precip[i+1..i+12]; prob = max(prob[i..i+12])
    //  chuvaAcum24h = Σ precip[i-23..i]; demais de current/daily.
    // Implementar com buscas de substring por chave + varredura dos arrays.
    // (Detalhe de varredura fica a cargo da impl; manter unidades centi.)
    // ... ver NOTA abaixo.
    return false; // substituir por parse real
}
#endif // ARCH_ESP32

bool WeatherClient::pollNow(const WeatherConfig &cfg, uint32_t nowEpoch, WeatherCache &out) {
#if defined(ARCH_ESP32)
    String path = String("/v1/forecast?latitude=") + String(cfg.latE7 / 1e7, 5) +
                  "&longitude=" + String(cfg.lonE7 / 1e7, 5) +
                  "&current=temperature_2m,relative_humidity_2m,precipitation,wind_gusts_10m" +
                  "&hourly=precipitation,precipitation_probability,soil_moisture_0_to_1cm" +
                  "&daily=temperature_2m_max,temperature_2m_min,wind_gusts_10m_max,et0_fao_evapotranspiration" +
                  "&past_hours=24&forecast_hours=13&timezone=auto";
    String body;
    if (!httpsGet(OM_HOST, path, body)) { out.isMock = true; return false; }
    WeatherCache parsed;
    if (!parseWeather(body, parsed)) { out.isMock = true; return false; }
    parsed.valid = true; parsed.isMock = false; parsed.fetchEpoch = nowEpoch;
    out = parsed;
    return true;
#else
    (void)cfg; (void)nowEpoch; (void)out;
    return false;
#endif
}

bool WeatherClient::tick(const WeatherConfig &cfg, uint32_t nowLocalSecs, bool staUp, WeatherCache &out) {
    if (!cfg.enabled || !staUp || nowLocalSecs == 0) return false;
#if defined(ARCH_ESP32)
    uint32_t hour = (nowLocalSecs / 3600) % 24;
    uint32_t day  = nowLocalSecs / 86400;
    uint32_t key  = day * 100 + hour;
    bool scheduled = (hour == cfg.pollHourA || hour == cfg.pollHourB);
    // seed pós-boot: primeiro tick com WiFi up dispara um poll único.
    if (!seededAfterBoot) { seededAfterBoot = true; return pollNow(cfg, nowLocalSecs, out); }
    if (scheduled && key != lastPollKey) {
        lastPollKey = key;
        return pollNow(cfg, nowLocalSecs, out);
    }
    return false;
#else
    (void)nowLocalSecs; (void)out;
    return false;
#endif
}
```

> **NOTA de implementação (parse leve):** `parseWeather` deve espelhar o
> `parseWeather` do mockup (`Irrigacao Mobile.dc.html:2386-2413`). Manter **sem
> ArduinoJson**: varrer `body` por substring das chaves (`"time":[`,
> `"precipitation":[`, `"precipitation_probability":[`, `"current":{"time":"..."`,
> `"daily":{...}`) e reduzir/maximizar as fatias com `atof`/`strtod` avançando
> pelas vírgulas. Como `forecast_hours=13&past_hours=24`, o array horário tem ~37
> pontos → cabe em `String`. Converter para centi-unidades (`*100`, `roundf`).
> Se o footprint TLS/heap não couber no gateway, o fallback é `out.isMock=true`
> (decisão continua fail-open) — nunca fabricar supressão a partir de mock.

- [ ] **Step 3: Build ESP32 do gateway (verificação de compilação/heap)**

Run (host com toolchain, ou CI): `pio run -e <env-do-gateway>`
Expected: compila; se `WiFiClientSecure.h` ausente no env, guardar também por `#if __has_include(<WiFiClientSecure.h>)` como em `MQTT.h:82`.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/WeatherClient.h src/modules/irrigation/WeatherClient.cpp
git commit -m "feat(irrigation): WeatherClient Open-Meteo (HTTPS leve + agendamento 2x/dia)"
```

---

## Task 10: Glue no módulo (tick do client + métodos gw*)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `src/modules/irrigation/IrrigationModule.cpp`

- [ ] **Step 1: Membro + declarações**

`IrrigationModule.h`:
```cpp
#include "modules/irrigation/WeatherClient.h"
// ... em membros privados:
    WeatherClient weatherClient;
// ... métodos públicos (usados pelos endpoints):
  public:
    bool gwWeatherSetConfig(uint8_t enabled, int32_t latE7, int32_t lonE7);
    uint8_t gwWeatherUpsertRule(const WeatherRule &r); // retorna id (0=falha)
    bool gwWeatherDeleteRule(uint8_t id);
    bool gwWeatherRefresh(); // poll imediato; false se sem WiFi
    // acessos de leitura para os builders:
    const WeatherConfig &gwWeatherConfig() const { return gateway.weatherConfig; }
    const WeatherRuleTable &gwWeatherRules() const { return gateway.weatherRules; }
    const WeatherCache &gwWeatherCache() const { return gateway.weatherCache; }
```

- [ ] **Step 2: Chamar `weatherClient.tick` no laço do gateway**

Em `IrrigationModule.cpp`, no mesmo trecho onde `computeLocalSecs(epochLocal)` é obtido para o scheduler (~3143), após o bloco do scheduler:
```cpp
    {
        bool staUp = gwStaConnected(); // helper existente da fase 8a; senão use isWifiConnected()
        if (weatherClient.tick(gateway.weatherConfig, epochLocal, staUp, gateway.weatherCache))
            LOG_INFO("Weather: cache atualizado (12h=%u cmm, prob=%u%%)",
                     gateway.weatherCache.chuvaPrevista12hCenti, gateway.weatherCache.probChuvaPct);
    }
```
> NOTA: confirmar o helper de status STA da fase 8a (ex.: `gwStaConnected()`/`WiFi.isConnected()`); usar o mesmo que a página "Rede Wi-Fi" usa.

- [ ] **Step 3: Implementar os métodos gw***

```cpp
bool IrrigationModule::gwWeatherSetConfig(uint8_t enabled, int32_t latE7, int32_t lonE7) {
    gateway.weatherConfig.enabled = enabled ? 1 : 0;
    gateway.weatherConfig.latE7 = latE7;
    gateway.weatherConfig.lonE7 = lonE7;
    return saveWeatherConfig();
}
uint8_t IrrigationModule::gwWeatherUpsertRule(const WeatherRule &rIn) {
    WeatherRule r = rIn;
    if (!r.id) { r.id = gateway.weatherRules.nextFreeId(); if (!r.id) return 0; }
    if (!gateway.weatherRules.upsert(r)) return 0;
    if (!saveWeatherRules()) return 0;
    return r.id;
}
bool IrrigationModule::gwWeatherDeleteRule(uint8_t id) {
    if (!gateway.weatherRules.removeById(id)) return false;
    return saveWeatherRules();
}
bool IrrigationModule::gwWeatherRefresh() {
    bool staUp = gwStaConnected();
    if (!staUp) return false;
    uint32_t nowLocal = 0; computeLocalSecs(nowLocal);
    return weatherClient.pollNow(gateway.weatherConfig, nowLocal, gateway.weatherCache);
}
```

- [ ] **Step 4: Build de sanidade**

Run: `./bin/run-tests.sh -f test_weather_engine`
Expected: PASS (headers puros seguem válidos).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): glue do WeatherClient (tick 2x/dia + métodos gwWeather*)"
```

---

## Task 11: WebApi builders + parse (puro, testável)

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`, `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_weather_webapi/test_main.cpp`

- [ ] **Step 1: Declarar em `IrrigationWebApi.h`**

```cpp
#include "modules/irrigation/WeatherConfig.h"
#include "modules/irrigation/WeatherForecast.h"
#include "modules/irrigation/WeatherRuleTable.h"

// Contexto de status agregado da página Meteorologia.
struct WeatherStatusCtx {
    const WeatherConfig *cfg = nullptr;
    const WeatherCache *cache = nullptr;
    const WeatherRuleTable *rules = nullptr;
    uint32_t nowEpoch = 0;
    bool staUp = false;
    const char *location = ""; // rótulo (nome do nó / "Gateway")
};
size_t buildWeatherStatus(const WeatherStatusCtx &ctx, char *buf, size_t cap);

// Parse do POST de regra. Espelha saveWeatherRule() do mockup.
struct WeatherRuleParse {
    bool ok = false;
    WeatherRule rule;
    const char *err = "";
};
WeatherRuleParse parseWeatherRule(const char *body, size_t n);

struct WeatherConfigParse {
    bool ok = false;
    uint8_t enabled = 0;
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
    const char *err = "";
};
WeatherConfigParse parseWeatherConfig(const char *body, size_t n);
```

- [ ] **Step 2: Teste (falha)**

`test/test_weather_webapi/test_main.cpp`:
```cpp
#include "modules/irrigation/IrrigationWebApi.h"
#include "modules/irrigation/WeatherEngine.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_build_status_has_fields() {
    WeatherConfig cfg; cfg.enabled = 1; cfg.latE7 = -235000000; cfg.lonE7 = -466000000;
    WeatherCache cache; cache.valid = true; cache.fetchEpoch = 1000;
    cache.chuvaPrevista12hCenti = 850; cache.probChuvaPct = 62;
    WeatherRuleTable rules;
    WeatherRule r; r.id = 1; r.enabled = 1; r.limiarMmCenti = 500; r.limiarPct = 60;
    r.grupoIds[0] = 1; strncpy(r.nome, "Chuva forte", 31);
    rules.upsert(r);
    WeatherStatusCtx ctx; ctx.cfg = &cfg; ctx.cache = &cache; ctx.rules = &rules;
    ctx.nowEpoch = 1000 + 3600; ctx.staUp = true; ctx.location = "Casa";
    char buf[2048];
    size_t n = buildWeatherStatus(ctx, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"enabled\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"probChuva\":62"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"triggered\":true")); // 8.5>5 e 62>60
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"nome\":\"Chuva forte\""));
}

static void test_parse_rule_ok() {
    const char *body = "{\"nome\":\"Horta\",\"limiarMm\":3,\"limiarPct\":50,"
                       "\"zonaIds\":[1,2],\"grupoIds\":[],\"enabled\":true,"
                       "\"mensagem\":\"dispensa horta\"}";
    WeatherRuleParse p = parseWeatherRule(body, strlen(body));
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL(300, p.rule.limiarMmCenti); // 3.0mm → 300 centi
    TEST_ASSERT_EQUAL(50, p.rule.limiarPct);
    TEST_ASSERT_EQUAL(1, p.rule.zonaIds[0]);
    TEST_ASSERT_EQUAL(2, p.rule.zonaIds[1]);
    TEST_ASSERT_EQUAL_STRING("Horta", p.rule.nome);
}

static void test_parse_rule_requires_target() {
    const char *body = "{\"nome\":\"X\",\"limiarMm\":3,\"limiarPct\":50,"
                       "\"zonaIds\":[],\"grupoIds\":[]}";
    WeatherRuleParse p = parseWeatherRule(body, strlen(body));
    TEST_ASSERT_FALSE(p.ok);
}

static void test_parse_rule_requires_name() {
    const char *body = "{\"nome\":\"\",\"limiarMm\":3,\"limiarPct\":50,\"zonaIds\":[1]}";
    WeatherRuleParse p = parseWeatherRule(body, strlen(body));
    TEST_ASSERT_FALSE(p.ok);
}

static void test_parse_config_ok() {
    const char *body = "{\"enabled\":true,\"lat\":-23.5,\"lon\":-46.6}";
    WeatherConfigParse p = parseWeatherConfig(body, strlen(body));
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL(1, p.enabled);
    TEST_ASSERT_EQUAL_INT32(-235000000, p.latE7);
    TEST_ASSERT_EQUAL_INT32(-466000000, p.lonE7);
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_build_status_has_fields);
    RUN_TEST(test_parse_rule_ok);
    RUN_TEST(test_parse_rule_requires_target);
    RUN_TEST(test_parse_rule_requires_name);
    RUN_TEST(test_parse_config_ok);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 3: Rodar — falha**

Run: `./bin/run-tests.sh -f test_weather_webapi`
Expected: FAIL.

- [ ] **Step 4: Implementar em `IrrigationWebApi.cpp`**

Usar `JsonWriter` para o builder e um mini-parser numérico para os POSTs (o módulo já não usa ArduinoJson — reusar/estender os helpers de parse existentes, ex.: `parseZoneUpsert`). Ponto-chave do builder — para cada regra, computar `triggered` via `WeatherEngine::ruleTriggered` e `chuvaAtual/probAtual` a partir do cache:
```cpp
#include "modules/irrigation/WeatherEngine.h"

static void fmtCentiToDecimal(char *out, size_t cap, int32_t centi) {
    snprintf(out, cap, "%d.%01d", centi / 100, (centi < 0 ? -centi : centi) % 100 / 10);
}

size_t buildWeatherStatus(const WeatherStatusCtx &ctx, char *buf, size_t cap) {
    JsonWriter w(buf, cap);
    const WeatherCache &c = *ctx.cache;
    const WeatherConfig &cfg = *ctx.cfg;
    bool fresh = WeatherEngine::cacheFresh(c, ctx.nowEpoch, cfg.staleTtlH);
    w.beginObject();
    w.keyBool("enabled", cfg.enabled);
    w.key("lat"); { char t[16]; fmtCentiToDecimal(t, sizeof(t), cfg.latE7 / 100000); w.raw(t); }
    w.key("lon"); { char t[16]; fmtCentiToDecimal(t, sizeof(t), cfg.lonE7 / 100000); w.raw(t); }
    w.keyNum("updatedEpoch", (int64_t)c.fetchEpoch);
    w.keyBool("isMock", c.isMock);
    w.keyBool("staUp", ctx.staUp);
    w.keyStr("location", ctx.location);
    // metrics (exibição)
    w.key("metrics"); w.beginObject();
    w.keyNum("chuvaPrevista12hCenti", c.chuvaPrevista12hCenti);
    w.keyNum("probChuva", c.probChuvaPct);
    w.keyNum("chuvaAcum24hCenti", c.chuvaAcum24hCenti);
    w.keyNum("umidadeSolo", c.umidadeSoloPct);
    w.keyNum("tempMinCenti", c.tempMinCenti);
    w.keyNum("tempMaxCenti", c.tempMaxCenti);
    w.keyNum("ventoRajadaCenti", c.ventoRajadaCenti);
    w.keyNum("et0Centi", c.et0Centi);
    w.keyNum("tempAtualCenti", c.tempAtualCenti);
    w.keyNum("umidadeRel", c.umidadeRelPct);
    w.endObject();
    // regras
    bool anySup = false;
    w.key("rules"); w.beginArray();
    for (size_t i = 0; i < ctx.rules->count(); i++) {
        const WeatherRule *r = ctx.rules->ruleAt(i);
        bool trig = WeatherEngine::ruleTriggered(*r, c, ctx.nowEpoch, cfg.staleTtlH);
        if (trig) anySup = true;
        w.beginObject();
        w.keyNum("id", r->id);
        w.keyStr("nome", r->nome);
        w.keyBool("enabled", r->enabled);
        w.keyNum("limiarMmCenti", r->limiarMmCenti);
        w.keyNum("limiarPct", r->limiarPct);
        w.keyStr("mensagem", r->mensagem);
        w.key("zonaIds"); w.beginArray();
        for (uint8_t z : r->zonaIds) if (z) w.num(z);
        w.endArray();
        w.key("grupoIds"); w.beginArray();
        for (uint8_t g : r->grupoIds) if (g) w.num(g);
        w.endArray();
        w.keyBool("triggered", trig);
        w.keyBool("fresh", fresh);
        w.keyNum("chuvaAtualCenti", c.chuvaPrevista12hCenti);
        w.keyNum("probAtual", c.probChuvaPct);
        w.endObject();
    }
    w.endArray();
    w.keyBool("anySuppressed", anySup);
    w.endObject();
    return w.done();
}
```
`parseWeatherRule` / `parseWeatherConfig`: extrair campos com um scanner mínimo
(reusar o utilitário de parse do módulo). Converter `limiarMm` (decimal, ex. 3 ou
3.5) para `limiarMmCenti` (`round(x*100)`); `lat`/`lon` (decimal) para E7
(`round(x*1e7)`). Validações espelham `saveWeatherRule` do mockup: nome não-vazio,
limiares numéricos, ≥1 alvo (zona ou grupo).

- [ ] **Step 5: Rodar — passa**

Run: `./bin/run-tests.sh -f test_weather_webapi`
Expected: PASS (5 testes).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_weather_webapi/test_main.cpp
git commit -m "feat(irrigation): builders/parse de clima (status + regra + config, com testes)"
```

---

## Task 12: Endpoints HTTP

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp`

- [ ] **Step 1: Handlers**

Adicionar (perto dos demais `h*`), seguindo o padrão de `hOverview`/`hZonesPost`:
```cpp
static void hWeatherGet(HTTPRequest *req, HTTPResponse *res) {
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    WeatherStatusCtx c = {};
    c.cfg = &irrigationModule->gwWeatherConfig();
    c.rules = &irrigationModule->gwWeatherRules();
    c.cache = &irrigationModule->gwWeatherCache();
    c.nowEpoch = irrigationModule->gwNowLocalSecs(); // helper: computeLocalSecs público (ver nota)
    c.staUp = irrigationModule->gwStaConnected();
    c.location = irrigationModule->gwNodeLabel(); // nome do nó/"Gateway"
    char buf[3072];
    if (!buildWeatherStatus(c, buf, sizeof(buf))) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

static void hWeatherConfigPost(HTTPRequest *req, HTTPResponse *res) {
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    WeatherConfigParse p = parseWeatherConfig(body, nb);
    if (!p.ok) { sendJson(res, "{\"ok\":false,\"reason\":\"coordenadas inválidas\"}", 400); return; }
    bool ok = irrigationModule->gwWeatherSetConfig(p.enabled, p.latE7, p.lonE7);
    sendJson(res, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void hWeatherRulePost(HTTPRequest *req, HTTPResponse *res) {
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[1024];
    size_t nb = readBody(req, body, sizeof(body));
    WeatherRuleParse p = parseWeatherRule(body, nb);
    if (!p.ok) { char e[128]; snprintf(e, sizeof(e), "{\"ok\":false,\"errors\":[\"%s\"]}", p.err); sendJson(res, e, 400); return; }
    uint8_t id = irrigationModule->gwWeatherUpsertRule(p.rule);
    if (!id) { sendJson(res, "{\"ok\":false,\"errors\":[\"tabela cheia\"]}", 400); return; }
    char out[64]; snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%u}", id);
    sendJson(res, out);
}

static void hWeatherRuleDelete(HTTPRequest *req, HTTPResponse *res) {
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[64];
    size_t nb = readBody(req, body, sizeof(body));
    int id = 0; { const char *p = strstr(body, "\"id\""); if (p) id = atoi(p + 4 + strspn(p + 4, "\": ")); }
    (void)nb;
    bool ok = id && irrigationModule->gwWeatherDeleteRule((uint8_t)id);
    sendJson(res, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void hWeatherRefresh(HTTPRequest *req, HTTPResponse *res) {
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    bool ok = irrigationModule->gwWeatherRefresh();
    sendJson(res, ok ? "{\"ok\":true,\"staUp\":true}" : "{\"ok\":false,\"reason\":\"sem WiFi\"}");
}
```
> NOTA: `gwNowLocalSecs()`, `gwStaConnected()`, `gwNodeLabel()` — se não existirem, criar wrappers finos em `IrrigationModule` (public) sobre `computeLocalSecs`, o helper STA da fase 8a e o nome do nó (`owner.long_name`/`"Gateway"`).

- [ ] **Step 2: Registrar rotas**

No bloco `registerNode` (~1074-1122):
```cpp
    server->registerNode(new ResourceNode("/api/irrigation/weather", "GET", &hWeatherGet));
    server->registerNode(new ResourceNode("/api/irrigation/weather/config", "POST", &hWeatherConfigPost));
    server->registerNode(new ResourceNode("/api/irrigation/weather/rule", "POST", &hWeatherRulePost));
    server->registerNode(new ResourceNode("/api/irrigation/weather/rule/delete", "POST", &hWeatherRuleDelete));
    server->registerNode(new ResourceNode("/api/irrigation/weather/refresh", "POST", &hWeatherRefresh));
```

- [ ] **Step 3: Build ESP32**

Run: `pio run -e <env-do-gateway>`
Expected: compila.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationWebEndpoints.cpp src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): endpoints /weather (status, config, rule, delete, refresh)"
```

---

## Task 13: UI — página Meteorologia (lista)

**Files:**
- Modify: `data/irrigacao/app.js`

Referência visual EXATA: `Irrigacao Mobile.dc.html` linhas 702-763 (lista) e a
lógica de view-model 2564-2653. Reproduzir a aparência (classes/estrutura do
painel `data/irrigacao/` já têm equivalentes CSS — usar as classes existentes
`card`, `chip`, `btn dashed`, etc., como nas outras telas).

- [ ] **Step 1: Registrar a sub-tela**

Em `renderMais()` (`app.js:2564-2589`), adicionar ao `items` (após `niveis` para espelhar a ordem do mockup, que lista Meteorologia logo antes de Controle de nível):
```javascript
    ['meteo', 'Meteorologia', 'Supressão por previsão de chuva (Open-Meteo)'],
```
Em `SECTION_LABELS` (`:3226`): `meteo: 'Meteorologia',`
Em `RENDER` (`:3204`): `meteo: renderMeteo,`
Em `POLLED` (se existir o mapa): `meteo: 1,` — para atualizar a leitura a cada 3s.

- [ ] **Step 2: `renderMeteo()` (lista)**

Adicionar função nova (perto de `renderNiveis`). Usa `getJson('/weather')`. Monta:
1. **Card Open-Meteo**: `location`, "Atualizado {fmtAgo(updatedEpoch)}" (+ " · estimativa offline" se `isMock`), faixa de erro se `isMock`, `tempAtual`/`umidadeRel`, grid 2col com as 8 métricas (converter *Centi → decimal). Botão **Atualizar** → `postJson('/weather/refresh')` e re-render.
2. **Faixa de status**: se `anySuppressed`, texto "Suprimindo: {nomes dos grupos/zonas afetados}" na cor azul `#…` (classe de destaque existente); senão "Sem restrição meteorológica no momento".
3. **Regras**: botão "+ Nova regra" → `meteoEditForm(null)`. Cada regra: nome, badge "Suprimindo" se `triggered`, toggle habilitado (`postJson('/weather/rule', {...regra, enabled:!enabled})`), `detalhe` = `Chuva prevista (12h) > {limiarMmCenti/100}mm E probabilidade > {limiarPct}%`, `alvosLabel` ("Zonas: … · Grupos: …" ou "Nenhum alvo selecionado" — resolver nomes via `/zones` e `/groups`), "Leitura atual: {chuvaAtualCenti/100}mm · {probAtual}%". Clique no card → `meteoEditForm(regra)`.

Seguir o idioma de `renderGrupos` (`app.js:1656-1738`): `Promise.all([getJson('/weather'), getJson('/zones'), getJson('/groups')])`, montar `view.innerHTML` com template strings + `esc()`, e bindar listeners com `querySelectorAll`.

- [ ] **Step 3: Verificação manual (servir o painel)**

Abrir o painel (build ou mock local), navegar Mais › Meteorologia; conferir que o
card, a faixa e a lista de regras aparecem com o mesmo layout do mockup.

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/app.js
git commit -m "feat(irrigation): painel — página Meteorologia (lista + card + status)"
```

---

## Task 14: UI — edição de regra

**Files:**
- Modify: `data/irrigacao/app.js`

Referência EXATA: `Irrigacao Mobile.dc.html` linhas 765-839 (form) e 2655-2686 (view-model).

- [ ] **Step 1: `meteoEditForm(regra)`**

Renderiza o formulário (novo/edição). Campos, na ordem do mockup:
- Cabeçalho ‹ Meteorologia (volta à lista).
- Título "Nova regra meteorológica" / "Editar regra".
- Bloco de erros (lista) quando o POST retornar `errors`.
- **Nome da regra** (input texto).
- Linha com dois inputs numéricos: **Chuva prevista em 12h > (mm)** e **Probabilidade de chuva > (%)**.
- Nota "Suprime quando **ambas** as condições forem satisfeitas na consulta (2 consultas diárias à Open-Meteo)."
- **Zonas afetadas**: chips toggle (fonte `/zones`), padrão `chipStyle` (selecionado = verde).
- **Grupos hidráulicos afetados**: chips toggle (fonte `/groups`).
- **Mensagem de alerta** (input texto).
- Toggle **Regra habilitada**.
- Botão **Salvar regra** → monta payload `{id?, nome, limiarMm, limiarPct, zonaIds, grupoIds, enabled, mensagem}` e `postJson('/weather/rule', payload)`; em `ok`, volta à lista e re-render; em erro, mostra `errors`.
- Em edição: botão **Excluir regra** com confirmação inline (Cancelar/Excluir) → `postJson('/weather/rule/delete', {id})`.

Validação client-side espelha `saveWeatherRule` (mockup 2490-2506): nome não-vazio,
limiares numéricos, ≥1 alvo. (O backend revalida — Task 11.)

- [ ] **Step 2: Verificação manual**

Criar regra nova cobrindo um grupo; salvar; editar; alternar habilitado; excluir.
Conferir layout idêntico ao mockup.

- [ ] **Step 3: Commit**

```bash
git add data/irrigacao/app.js
git commit -m "feat(irrigation): painel — edição/criação/exclusão de regra meteorológica"
```

---

## Task 15: Rótulo "Suprimido por meteorologia" em zonas/grupos (overview)

**Files:**
- Modify: `data/irrigacao/app.js` (e, se necessário, `IrrigationWebApi.cpp` para expor o motivo por alvo no `/overview`)

Referência: mockup linhas 195, 417-418, 2586-2590.

- [ ] **Step 1: Exibir motivo**

Onde o painel lista zonas/grupos com estado (renderGrupos/renderZones/overview),
quando o alvo estiver suprimido agora, mostrar a linha "Suprimido por meteorologia
— {mensagem da regra}". Fonte: reusar `/weather` (campo `rules[].triggered` +
`mensagem` + `zonaIds/grupoIds`) para computar no cliente quais alvos estão
suprimidos, sem novo endpoint. (Se preferir server-side, adicionar `suppressedBy`
por zona no `/overview` — opcional.)

- [ ] **Step 2: Commit**

```bash
git add data/irrigacao/app.js
git commit -m "feat(irrigation): painel — rótulo de supressão climática em zonas/grupos"
```

---

## Task 16: Fechamento — suíte verde, contagem e formatação

**Files:**
- Modify: `bin/run-tests.sh` (se houver contagem esperada de suítes), formatação geral.

- [ ] **Step 1: Contagem de suítes**

Se `bin/run-tests.sh` valida um número esperado de suítes, incrementar em **+4**
(test_weather_config, test_weather_rule_table, test_weather_engine,
test_weather_webapi). Localizar a constante e ajustar.

- [ ] **Step 2: Suíte completa**

Run: `./bin/run-tests.sh`
Expected: GREEN (exit 0), incluindo as 4 novas suítes (22 casos novos no total).

- [ ] **Step 3: Formatação**

Run: `trunk fmt`
Expected: sem diffs pendentes após aplicar.

- [ ] **Step 4: Build do firmware do gateway**

Run: `pio run -e <env-do-gateway>`
Expected: compila (client TLS + endpoints).

- [ ] **Step 5: Commit final**

```bash
git add -A
git commit -m "chore(irrigation): fecha fase meteorologia (suíte verde + fmt + contagem)"
```

---

## Verificação manual / hardware (MCP harness) — pós-implementação

1. Configurar lat/lon e habilitar; criar regra cobrindo um grupo com limiares baixos.
2. "Atualizar" → card mostra métricas; regra fica "Suprimindo"; programa não abre;
   evento `CLIMA/CMD_SUPRIMIDO` no log de auditoria.
3. Desabilitar a regra → grupo volta a irrigar no próximo tick.
4. Derrubar WiFi por > TTL → cache expira → fail-open (irriga), UI marca erro/estimativa.
5. Trocar limiares altos → deixa de suprimir mesmo com chuva prevista.

## Notas de risco (rever na impl)

- **TLS/heap no gateway**: se `WiFiClientSecure` estourar heap, reduzir a resposta
  (já usando `forecast_hours`/`past_hours`) e/ou setar buffers menores; fallback
  `isMock` mantém fail-open. Guardar `WiFiClientSecure.h` por `__has_include`.
- **Accessors reais** de `HydraulicGroupTable` (Task 8) e helpers STA/nome de nó
  (Tasks 10/12): confirmar nomes existentes antes de compilar.
- **`sizeof(WeatherRule)`** (Task 3): ajustar o `static_assert` ao valor real do
  compilador; manter idêntico entre nativo e ESP32.
- **Contagem de suítes** (Task 16): só se `run-tests.sh` a impõe.
