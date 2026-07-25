# Irrigação Fase 8c — Portal do device SERVICO — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dar interface ao device SERVICO — portal Wi-Fi com abas Clientes/Rede/Log (§11.8) + vias portal do import/export (§11.7), dirigindo o núcleo do 8b.

**Architecture:** Espelha o layering do repo (5b/7b): camada **pura** `ServicePortalApi` (build/parse, nativo-testada em `test_service_portal`) + **glue ESP32** `ServicePortalEndpoints.cpp` (rotas `/api/portal/service/*`, gated `role==SERVICO`) dirigindo `ServiceController`/`ServiceVault` do 8b via novos accessors `svcPortal*` do `IrrigationModule` + **frontend** `data/irrigacao/portal/` com tabs condicionais ao role.

**Tech Stack:** C++17, Unity (testes nativos), JSON hand-rolled (`IrrigationWeb::JsonWriter`/`JsonReader` + scanner estrutural `IrrigationService::json*`), esp32_https_server (glue ESP32), PlatformIO.

## Global Constraints

- **Protocolo VERSION permanece 1; ABI de settings v5 (176 B) intacta.** Nenhum tipo de wire novo; reusa GET/SET_CONFIG fragmentado (Fase 2), pulso/zona, PAIR_GRANT (Fase 3), `RESYNC_SEQ`/`PING_SURVEY` (8a) e os executores do 8b. Toda emissão marcada `FLAG_FROM_SERVICE` (executores do 8b já carimbam via `setServiceFlag`).
- **Rotas `/api/portal/service/*` gated `role==SERVICO`** — todo handler retorna 404 se `!irrigationModule || !irrigationModule->svcIsService()`.
- **Padrão de teste do repo:** lógica pura nativo-testada; glue de handler/rádio validado por **compilação nativa + banca de hardware** (nenhum teste nativo instancia `IrrigationModule`). **Banca 2+ nós EXIGIDA antes de campo.**
- **Rodar testes nativos:** canônico `./bin/run-tests.sh` (exit 0 GREEN) e iteração de suite `./bin/run-tests.sh -f test_service_portal` (exit 3 FILTERED). **Neste host Windows os testes rodam via Docker** — usar a incantação da memória `windows-native-test-docker` (MSYS_NO_PATHCONV=1, volumes nomeados, **excluir `.claude` e `myfork`** da cópia, copiar `platformio.ini`+`variants/`+`bin/` para o cache; `platformio test -e coverage -f <suite> -vv`). Primeira build de suite NOVA ≈ 8 min; incrementais ≈ 26 s.
- **Ao adicionar a suite nova:** incrementar `test/native-suite-count` (59 → 60), senão o runner reporta AMBER.
- **Ao adicionar a cola só-ESP32:** excluir `ServicePortalEndpoints.cpp` do build nativo em `variants/native/portduino.ini` (senão `#include <HTTPRequest.hpp>` não encontrado → ERRORED).
- **`trunk fmt` não roda no host Windows** — seguir o clang-format do repo; a CI do fork valida.
- **Commits:** `feat(irrigation): …` / `docs(irrigation): …`. Fim de mensagem: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- **Campos geridos do editor de config (read-only, preservados no parse):** `magic`, `version`, `role`, `boundGateway`, `configEpoch`. Todo o resto é editável.

---

## File Structure

**Criar:**
- `src/modules/irrigation/ServicePortalApi.h` — declarações da camada pura.
- `src/modules/irrigation/ServicePortalApi.cpp` — impl pura (build/parse).
- `src/modules/irrigation/ServicePortalEndpoints.h` — decl de `registerIrrigationServicePortalHandlers`.
- `src/modules/irrigation/ServicePortalEndpoints.cpp` — handlers ESP32 + registro (só-webserver).
- `test/test_service_portal/test_main.cpp` — suite nativa da camada pura.

**Modificar:**
- `test/native-suite-count` — 59 → 60.
- `variants/native/portduino.ini` — excluir `ServicePortalEndpoints.cpp` do nativo.
- `src/modules/irrigation/IProfileStore.h` — `IServiceLogReader` + append/tail de log no store; duplê RAM.
- `src/modules/irrigation/ServiceController.h` / `.cpp` — `logService(...)` + acesso ao reader.
- `src/modules/irrigation/LittleFsProfileStore.h` / `.cpp` — impl append/tail de `/log/servico.jsonl`.
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — accessors `svcPortal*` + intake GET_CONFIG.
- `src/mesh/http/ContentHandler.cpp` — chamar `registerIrrigationServicePortalHandlers` (2 sítios).
- `data/irrigacao/portal/index.html` / `app.js` — abas Clientes/Rede/Log condicionais a `role==SERVICO`.

---

## Task 1: Scaffold da suite pura + `buildClientList` (aba Clientes)

**Files:**
- Create: `src/modules/irrigation/ServicePortalApi.h`
- Create: `src/modules/irrigation/ServicePortalApi.cpp`
- Create: `test/test_service_portal/test_main.cpp`
- Modify: `test/native-suite-count`

**Interfaces:**
- Consumes: `IrrigationService::LightProfile` (de `ServiceBackup.h`), `IrrigationService::presetToString`, `IrrigationWeb::JsonWriter` (de `IrrigationWebApi.h`).
- Produces: `size_t IrrigationWeb::buildClientList(const IrrigationService::LightProfile *clients, size_t n, const char *activeId, char *buf, size_t cap)`.

- [ ] **Step 1: Criar o header com a primeira declaração**

`src/modules/irrigation/ServicePortalApi.h`:
```cpp
#pragma once
#include "modules/irrigation/IrrigationSettings.h"
#include "modules/irrigation/IrrigationWebApi.h" // JsonWriter/JsonReader/ParseResult (reuso)
#include "modules/irrigation/ServiceBackup.h"     // IrrigationService::LightProfile + scanner
#include "modules/irrigation/ServiceController.h" // ScanResults/ScanEntry
#include <cstddef>
#include <cstdint>

namespace IrrigationWeb
{

// ── Aba Clientes ─────────────────────────────────────────────────────────────
// [{id,nome,canal,preset,gateway,estacoes,active}] sob {"clients":[…]}.
size_t buildClientList(const IrrigationService::LightProfile *clients, size_t n, const char *activeId, char *buf,
                       size_t cap);

} // namespace IrrigationWeb
```

- [ ] **Step 2: Criar o .cpp com a impl**

`src/modules/irrigation/ServicePortalApi.cpp`:
```cpp
#include "modules/irrigation/ServicePortalApi.h"
#include <cstring>

namespace IrrigationWeb
{

size_t buildClientList(const IrrigationService::LightProfile *clients, size_t n, const char *activeId, char *buf,
                       size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("clients");
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const IrrigationService::LightProfile &c = clients[i];
        w.beginObject();
        w.keyStr("id", c.id);
        w.keyStr("nome", c.nome);
        w.keyStr("canal", c.canalNome);
        w.keyStr("preset", IrrigationService::presetToString(c.preset));
        w.keyNum("gateway", (int64_t)c.gateway);
        w.keyNum("estacoes", c.estacaoCount);
        bool active = activeId && activeId[0] && strcmp(activeId, c.id) == 0;
        w.keyBool("active", active);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}

} // namespace IrrigationWeb
```

- [ ] **Step 3: Escrever o teste que falha**

`test/test_service_portal/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ServicePortalApi.h"
#include <cstring>
#include <unity.h>

using namespace IrrigationWeb;

void setUp(void) {}
void tearDown(void) {}

static void test_buildClientList_marks_active()
{
    IrrigationService::LightProfile cs[2] = {};
    strcpy(cs[0].id, "f1");
    strcpy(cs[0].nome, "Sitio A");
    strcpy(cs[0].canalNome, "bv-irrig");
    cs[0].preset = 0; // LONG_FAST
    cs[0].gateway = 0xa1b2c3d4;
    cs[0].estacaoCount = 3;
    strcpy(cs[1].id, "f2");
    char buf[512];
    size_t n = buildClientList(cs, 2, "f2", buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":\"f1\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"canal\":\"bv-irrig\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active\":true"));  // f2 ativo
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active\":false")); // f1 inativo
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_buildClientList_marks_active);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 4: Registrar a suite no contador canônico**

Editar `test/native-suite-count`: trocar `59` por `60`.

- [ ] **Step 5: Rodar a suite (filtrada) e confirmar GREEN da lógica**

Run (via Docker, memória `windows-native-test-docker`): `./bin/run-tests.sh -f test_service_portal`
Expected: `RESULT: FILTERED 1/60 … filtered: test_service_portal` (exit 3) — a suite compila e o caso passa. (Primeira build da suite nova ≈ 8 min.)

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/ServicePortalApi.h src/modules/irrigation/ServicePortalApi.cpp \
        test/test_service_portal/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): 8c ServicePortalApi scaffold + buildClientList (aba Clientes)"
```

---

## Task 2: `parseSelect` (seleção de cliente)

**Files:**
- Modify: `src/modules/irrigation/ServicePortalApi.h`, `src/modules/irrigation/ServicePortalApi.cpp`
- Test: `test/test_service_portal/test_main.cpp`

**Interfaces:**
- Consumes: `IrrigationWeb::JsonReader`, `IrrigationWeb::ParseResult` (de `IrrigationWebApi.h`).
- Produces: `ParseResult IrrigationWeb::parseSelect(const char *json, size_t len, char *idOut, size_t idCap)`.

- [ ] **Step 1: Escrever o teste que falha**

Adicionar em `test_main.cpp` (e `RUN_TEST(test_parseSelect_reads_id);` no `setup`):
```cpp
static void test_parseSelect_reads_id()
{
    char id[32] = {0};
    ParseResult r = parseSelect("{\"id\":\"fazenda-sp-01\"}", 22, id, sizeof id);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("fazenda-sp-01", id);
    ParseResult r2 = parseSelect("{}", 2, id, sizeof id);
    TEST_ASSERT_FALSE(r2.ok);
}
```

- [ ] **Step 2: Rodar e confirmar que falha (símbolo indefinido)**

Run: `./bin/run-tests.sh -f test_service_portal`
Expected: RED — link/compile error `parseSelect` não declarado.

- [ ] **Step 3: Declarar no header**

Em `ServicePortalApi.h`, após `buildClientList`:
```cpp
// {"id":"<clientId>"} → idOut. Falha se ausente/vazio.
ParseResult parseSelect(const char *json, size_t len, char *idOut, size_t idCap);
```

- [ ] **Step 4: Implementar no .cpp**

Em `ServicePortalApi.cpp`:
```cpp
ParseResult parseSelect(const char *json, size_t len, char *idOut, size_t idCap)
{
    ParseResult r;
    JsonReader rd(json, len);
    if (!rd.getStr("id", idOut, idCap) || idOut[0] == '\0')
        r.fail("id ausente");
    return r;
}
```

- [ ] **Step 5: Rodar e confirmar GREEN da lógica**

Run: `./bin/run-tests.sh -f test_service_portal`
Expected: FILTERED (exit 3) — ambos os casos passam.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/ServicePortalApi.h src/modules/irrigation/ServicePortalApi.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): 8c parseSelect (seleção de cliente ativo)"
```

---

## Task 3: `buildScanResults` (aba Rede — respondentes)

**Files:**
- Modify: `src/modules/irrigation/ServicePortalApi.h`, `.cpp`
- Test: `test/test_service_portal/test_main.cpp`

**Interfaces:**
- Consumes: `ScanResults`/`ScanEntry` (de `ServiceController.h`).
- Produces: `size_t IrrigationWeb::buildScanResults(const ScanResults &scan, char *buf, size_t cap)`.

- [ ] **Step 1: Escrever o teste que falha** (`RUN_TEST(test_buildScanResults_maps_fields);`)
```cpp
static void test_buildScanResults_maps_fields()
{
    ScanResults s;
    ScanEntry e{};
    e.node = 0xe5f6a7b8;
    e.role = 0; // estação
    e.epoch = 17;
    e.vbatCentiV = 1240;
    e.fwVersion = 0x0800;
    e.lat = -221000000;
    e.lon = -476000000;
    e.snrQuarterDb = 32; // 8 dB
    s.add(e);
    char buf[1024];
    size_t n = buildScanResults(s, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"epoch\":17"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"vbat\":1240"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"snr\":32"));
}
```

- [ ] **Step 2: Rodar → RED** (`./bin/run-tests.sh -f test_service_portal`, `buildScanResults` indefinido).

- [ ] **Step 3: Declarar no header**
```cpp
// ── Aba Rede — varredura ─────────────────────────────────────────────────────
// {"nodes":[{node,role,epoch,vbat,fw,lat,lon,snr}]}.
size_t buildScanResults(const ScanResults &scan, char *buf, size_t cap);
```

- [ ] **Step 4: Implementar no .cpp**
```cpp
size_t buildScanResults(const ScanResults &scan, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("nodes");
    w.beginArray();
    for (size_t i = 0; i < scan.count(); i++) {
        const ScanEntry *e = scan.at(i);
        if (!e)
            break;
        w.beginObject();
        w.keyNum("node", (int64_t)e->node);
        w.keyNum("role", e->role);
        w.keyNum("epoch", (int64_t)e->epoch);
        w.keyNum("vbat", e->vbatCentiV);
        w.keyNum("fw", e->fwVersion);
        w.keyNum("lat", (int64_t)e->lat);
        w.keyNum("lon", (int64_t)e->lon);
        w.keyNum("snr", e->snrQuarterDb);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}
```

- [ ] **Step 5: Rodar → FILTERED (passa).**

- [ ] **Step 6: Commit**
```bash
git add src/modules/irrigation/ServicePortalApi.h src/modules/irrigation/ServicePortalApi.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): 8c buildScanResults (respondentes da varredura)"
```

---

## Task 4: `buildStationConfig` (editor de config — serializador)

**Files:**
- Modify: `src/modules/irrigation/ServicePortalApi.h`, `.cpp`
- Test: `test/test_service_portal/test_main.cpp`

**Interfaces:**
- Consumes: `IrrigationSettings` (de `IrrigationSettings.h`).
- Produces: `size_t IrrigationWeb::buildStationConfig(const IrrigationSettings &s, char *buf, size_t cap)`.

- [ ] **Step 1: Escrever o teste que falha** (`RUN_TEST(test_buildStationConfig_emits_fields);`)
```cpp
static void test_buildStationConfig_emits_fields()
{
    IrrigationSettings s; // defaults v5
    s.numValves = 3;
    s.hbMinutes = 12;
    s.pinsHbridgeA[0] = 4;
    s.sensores[0].pino = 34;
    s.sensores[0].unidade = 1; // bar
    s.localInterlocks[0].sensorIdx = 0;
    s.localInterlocks[0].saidasValvMask = 0x01;
    char buf[3072];
    size_t n = buildStationConfig(s, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"numValves\":3"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"hbMinutes\":12"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sensores\":["));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"localInterlocks\":["));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"role\":0")); // informativo read-only
}
```

- [ ] **Step 2: Rodar → RED.**

- [ ] **Step 3: Declarar no header**
```cpp
// ── Aba Rede — editor de config (settings v5 inteiro) ────────────────────────
// Serializa o blob v5. version/role/configEpoch/boundGateway saem como informativos
// (read-only no editor); parseStationConfig os preserva do `out` de entrada.
size_t buildStationConfig(const IrrigationSettings &s, char *buf, size_t cap);
```

- [ ] **Step 4: Implementar no .cpp** (adicionar helper de array de pinos no anonymous namespace no topo do arquivo)
```cpp
namespace
{
void writePinArray(JsonWriter &w, const char *key, const int8_t *pins, size_t n)
{
    w.key(key);
    w.beginArray();
    for (size_t i = 0; i < n; i++)
        w.num(pins[i]);
    w.endArray();
}
} // namespace

size_t buildStationConfig(const IrrigationSettings &s, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    // Informativos (read-only): parse os preserva do out.
    w.keyNum("version", s.version);
    w.keyNum("role", s.role);
    w.keyNum("configEpoch", (int64_t)s.configEpoch);
    w.keyNum("boundGateway", (int64_t)s.boundGateway);
    // Escalares editáveis.
    w.keyNum("numValves", s.numValves);
    w.keyNum("hbMinutes", s.hbMinutes);
    w.keyNum("vbatMinAbrirCentiV", s.vbatMinAbrirCentiV);
    w.keyNum("maxOpenConfigS", s.maxOpenConfigS);
    w.keyNum("cmdRatePerMin", s.cmdRatePerMin);
    w.keyNum("pulseMs", s.pulseMs);
    writePinArray(w, "pinsHbridgeA", s.pinsHbridgeA, IrrigationSettings::MAX_VALVES);
    writePinArray(w, "pinsHbridgeB", s.pinsHbridgeB, IrrigationSettings::MAX_VALVES);
    writePinArray(w, "pinsDigitalIn", s.pinsDigitalIn, IrrigationSettings::MAX_DIGITAL_IN);
    w.keyNum("digitalInActiveLow", s.digitalInActiveLow);
    w.keyNum("pinBtn", s.pinBtn);
    w.keyNum("pinLed", s.pinLed);
    writePinArray(w, "pinsGpo", s.pinsGpo, IrrigationSettings::MAX_GPO);
    w.keyNum("pinTamper", s.pinTamper);
    w.keyNum("hwFlags", s.hwFlags);
    w.keyNum("latE7", (int64_t)s.latE7);
    w.keyNum("lonE7", (int64_t)s.lonE7);
    w.key("sensores");
    w.beginArray();
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        const IrrigationSettings::SensorSlot &se = s.sensores[i];
        w.beginObject();
        w.keyNum("pino", se.pino);
        w.keyNum("tipo", se.tipo);
        w.keyNum("flags", se.flags);
        w.keyNum("amostragemS", se.amostragemS);
        w.keyNum("debounceMs", se.debounceMs);
        w.keyNum("adcMin", se.adcMin);
        w.keyNum("adcMax", se.adcMax);
        w.keyNum("engMin", se.engMin);
        w.keyNum("engMax", se.engMax);
        w.keyNum("unidade", se.unidade);
        w.endObject();
    }
    w.endArray();
    w.key("localInterlocks");
    w.beginArray();
    for (uint8_t i = 0; i < IrrigationSettings::MAX_LOCAL_INTERLOCKS; i++) {
        const IrrigationSettings::LocalInterlock &il = s.localInterlocks[i];
        w.beginObject();
        w.keyNum("sensorIdx", il.sensorIdx);
        w.keyNum("condicao", il.condicao);
        w.keyNum("acao", il.acao);
        w.keyNum("saidasValvMask", il.saidasValvMask);
        w.keyNum("valorCenti", (int64_t)il.valorCenti);
        w.keyNum("histereseCenti", il.histereseCenti);
        w.keyNum("saidasGpoMask", il.saidasGpoMask);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}
```

- [ ] **Step 5: Rodar → FILTERED (passa).**

- [ ] **Step 6: Commit**
```bash
git add src/modules/irrigation/ServicePortalApi.h src/modules/irrigation/ServicePortalApi.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): 8c buildStationConfig (serializa settings v5 p/ editor)"
```

---

## Task 5: `parseStationConfig` + round-trip anchor (editor de config — parser)

**Files:**
- Modify: `src/modules/irrigation/ServicePortalApi.h`, `.cpp`
- Test: `test/test_service_portal/test_main.cpp`

**Interfaces:**
- Consumes: scanner estrutural `IrrigationService::jsonMember`/`jsonForEachArray`/`jsonInt` (de `ServiceBackup.h`), `buildStationConfig` (Task 4).
- Produces: `ParseResult IrrigationWeb::parseStationConfig(const char *json, size_t len, IrrigationSettings &out)` — reescreve os campos editáveis; **preserva** `magic/version/role/boundGateway/configEpoch` de `out`.

- [ ] **Step 1: Escrever o teste âncora que falha** (`RUN_TEST(test_stationConfig_round_trip);`)
```cpp
static void test_stationConfig_round_trip()
{
    IrrigationSettings a; // defaults v5
    a.numValves = 5;
    a.hbMinutes = 7;
    a.vbatMinAbrirCentiV = 1205;
    a.maxOpenConfigS = 90;
    a.cmdRatePerMin = 4;
    a.pulseMs = 80;
    for (int i = 0; i < 8; i++) {
        a.pinsHbridgeA[i] = (int8_t)(10 + i);
        a.pinsHbridgeB[i] = (int8_t)(20 + i);
    }
    a.pinsDigitalIn[0] = 33;
    a.digitalInActiveLow = 0x03;
    a.pinBtn = 39;
    a.pinLed = 2;
    a.pinsGpo[0] = 25;
    a.pinsGpo[1] = 26;
    a.pinTamper = 27;
    a.hwFlags = 1;
    a.latE7 = -221000000;
    a.lonE7 = -476000000;
    a.sensores[0] = {34, 1, 0, 30, 0, 100, 4000, 0, 1000, 1, 0};
    a.sensores[2] = {35, 0, 1, 0, 200, 0, 4095, 0, 0, 0, 0};
    a.localInterlocks[0] = {0, 2, 1, 0x01, 1500, 50, 0x00, 0};
    a.localInterlocks[3] = {1, 3, 0, 0x04, -200, 10, 0x02, 0};
    // Campos geridos distintos p/ provar preservação (não vêm do JSON):
    a.role = (uint8_t)IrrigationRole::ESTACAO;
    a.boundGateway = 0xdeadbeef;
    a.configEpoch = 42;

    char js[3072];
    size_t n = buildStationConfig(a, js, sizeof js);
    TEST_ASSERT_TRUE(n > 0);

    IrrigationSettings b; // defaults; semeia só os geridos com os de `a`
    b.magic = a.magic;
    b.version = a.version;
    b.role = a.role;
    b.boundGateway = a.boundGateway;
    b.configEpoch = a.configEpoch;
    ParseResult r = parseStationConfig(js, n, b);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(IrrigationSettings)); // 176 B idênticos
}
```

- [ ] **Step 2: Rodar → RED** (`parseStationConfig` indefinido).

- [ ] **Step 3: Declarar no header**
```cpp
// Reconstrói os campos editáveis do blob a partir do JSON do editor. PRESERVA
// magic/version/role/boundGateway/configEpoch do `out` (o chamador semeia com o blob lido).
// Tolerante: campo ausente mantém o valor de `out`. Falha só com JSON vazio.
ParseResult parseStationConfig(const char *json, size_t len, IrrigationSettings &out);
```

- [ ] **Step 4: Implementar no .cpp** (adicionar `#include <cstdlib>` no topo; helpers no anonymous namespace)
```cpp
// (topo do arquivo)
#include "modules/irrigation/ServiceBackup.h" // scanner estrutural
#include <cstdlib>

// (dentro do anonymous namespace, junto de writePinArray)
using IrrigationService::Slice;

struct PinArrayCtx {
    int8_t *dst;
    size_t cap;
    size_t i;
};
bool fillPinCb(void *ctx, Slice elem)
{
    PinArrayCtx *c = static_cast<PinArrayCtx *>(ctx);
    if (c->i >= c->cap)
        return false;
    c->dst[c->i++] = (int8_t)strtol(elem.p, nullptr, 10); // elem.p aponta pro início do número
    return true;
}
void parsePinArray(const char *json, size_t n, const char *key, int8_t *dst, size_t cap)
{
    Slice arr;
    if (!IrrigationService::jsonMember(json, n, key, arr))
        return; // ausente → mantém out
    PinArrayCtx c{dst, cap, 0};
    IrrigationService::jsonForEachArray(arr, &c, fillPinCb);
}

struct SensorCtx {
    IrrigationSettings::SensorSlot *dst;
    size_t i;
};
bool sensorCb(void *ctx, Slice elem)
{
    SensorCtx *c = static_cast<SensorCtx *>(ctx);
    if (c->i >= IrrigationSettings::MAX_SENSORS)
        return false;
    IrrigationSettings::SensorSlot &se = c->dst[c->i++];
    int64_t v;
    if (IrrigationService::jsonInt(elem, "pino", v)) se.pino = (int8_t)v;
    if (IrrigationService::jsonInt(elem, "tipo", v)) se.tipo = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "flags", v)) se.flags = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "amostragemS", v)) se.amostragemS = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "debounceMs", v)) se.debounceMs = (uint16_t)v;
    if (IrrigationService::jsonInt(elem, "adcMin", v)) se.adcMin = (uint16_t)v;
    if (IrrigationService::jsonInt(elem, "adcMax", v)) se.adcMax = (uint16_t)v;
    if (IrrigationService::jsonInt(elem, "engMin", v)) se.engMin = (int16_t)v;
    if (IrrigationService::jsonInt(elem, "engMax", v)) se.engMax = (int16_t)v;
    if (IrrigationService::jsonInt(elem, "unidade", v)) se.unidade = (uint8_t)v;
    return true;
}

struct InterlockCtx {
    IrrigationSettings::LocalInterlock *dst;
    size_t i;
};
bool interlockCb(void *ctx, Slice elem)
{
    InterlockCtx *c = static_cast<InterlockCtx *>(ctx);
    if (c->i >= IrrigationSettings::MAX_LOCAL_INTERLOCKS)
        return false;
    IrrigationSettings::LocalInterlock &il = c->dst[c->i++];
    int64_t v;
    if (IrrigationService::jsonInt(elem, "sensorIdx", v)) il.sensorIdx = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "condicao", v)) il.condicao = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "acao", v)) il.acao = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "saidasValvMask", v)) il.saidasValvMask = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "valorCenti", v)) il.valorCenti = (int32_t)v;
    if (IrrigationService::jsonInt(elem, "histereseCenti", v)) il.histereseCenti = (uint16_t)v;
    if (IrrigationService::jsonInt(elem, "saidasGpoMask", v)) il.saidasGpoMask = (uint8_t)v;
    return true;
}
```
E a função (fora do anonymous namespace):
```cpp
ParseResult parseStationConfig(const char *json, size_t len, IrrigationSettings &out)
{
    ParseResult r;
    if (!json || len == 0) {
        r.fail("config vazia");
        return r;
    }
    Slice top{json, len};
    int64_t v;
    // Escalares editáveis (ausente → mantém out).
    if (IrrigationService::jsonInt(top, "numValves", v)) out.numValves = (uint8_t)v;
    if (IrrigationService::jsonInt(top, "hbMinutes", v)) out.hbMinutes = (uint16_t)v;
    if (IrrigationService::jsonInt(top, "vbatMinAbrirCentiV", v)) out.vbatMinAbrirCentiV = (uint16_t)v;
    if (IrrigationService::jsonInt(top, "maxOpenConfigS", v)) out.maxOpenConfigS = (uint16_t)v;
    if (IrrigationService::jsonInt(top, "cmdRatePerMin", v)) out.cmdRatePerMin = (uint8_t)v;
    if (IrrigationService::jsonInt(top, "pulseMs", v)) out.pulseMs = (uint16_t)v;
    if (IrrigationService::jsonInt(top, "digitalInActiveLow", v)) out.digitalInActiveLow = (uint8_t)v;
    if (IrrigationService::jsonInt(top, "pinBtn", v)) out.pinBtn = (int8_t)v;
    if (IrrigationService::jsonInt(top, "pinLed", v)) out.pinLed = (int8_t)v;
    if (IrrigationService::jsonInt(top, "pinTamper", v)) out.pinTamper = (int8_t)v;
    if (IrrigationService::jsonInt(top, "hwFlags", v)) out.hwFlags = (uint8_t)v;
    if (IrrigationService::jsonInt(top, "latE7", v)) out.latE7 = (int32_t)v;
    if (IrrigationService::jsonInt(top, "lonE7", v)) out.lonE7 = (int32_t)v;
    // Arrays de pinos.
    parsePinArray(json, len, "pinsHbridgeA", out.pinsHbridgeA, IrrigationSettings::MAX_VALVES);
    parsePinArray(json, len, "pinsHbridgeB", out.pinsHbridgeB, IrrigationSettings::MAX_VALVES);
    parsePinArray(json, len, "pinsDigitalIn", out.pinsDigitalIn, IrrigationSettings::MAX_DIGITAL_IN);
    parsePinArray(json, len, "pinsGpo", out.pinsGpo, IrrigationSettings::MAX_GPO);
    // Sensores + intertravamentos.
    Slice arr;
    if (IrrigationService::jsonMember(json, len, "sensores", arr)) {
        SensorCtx sc{out.sensores, 0};
        IrrigationService::jsonForEachArray(arr, &sc, sensorCb);
    }
    if (IrrigationService::jsonMember(json, len, "localInterlocks", arr)) {
        InterlockCtx ic{out.localInterlocks, 0};
        IrrigationService::jsonForEachArray(arr, &ic, interlockCb);
    }
    return r; // magic/version/role/boundGateway/configEpoch intocados (preservados)
}
```

- [ ] **Step 5: Rodar → FILTERED (round-trip passa: 176 B idênticos).**

Run: `./bin/run-tests.sh -f test_service_portal`
Expected: FILTERED (exit 3) — `test_stationConfig_round_trip` passa (`TEST_ASSERT_EQUAL_MEMORY`).

- [ ] **Step 6: Commit**
```bash
git add src/modules/irrigation/ServicePortalApi.h src/modules/irrigation/ServicePortalApi.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): 8c parseStationConfig + round-trip âncora do blob v5"
```

---

## Task 6: `parseNodeConfigReq` (envelope de escrita: node+rota+config)

**Files:**
- Modify: `src/modules/irrigation/ServicePortalApi.h`, `.cpp`
- Test: `test/test_service_portal/test_main.cpp`

**Interfaces:**
- Consumes: `parseStationConfig` (Task 5), `IrrigationService::jsonStr`/`jsonMember`/`parseNodeHex`.
- Produces: `enum class SvcRoute`, `struct NodeConfigReq { uint32_t node; SvcRoute route; IrrigationSettings config; }`, `ParseResult IrrigationWeb::parseNodeConfigReq(const char *json, size_t len, NodeConfigReq &out)`.

- [ ] **Step 1: Escrever o teste que falha** (`RUN_TEST(test_parseNodeConfigReq);`)
```cpp
static void test_parseNodeConfigReq()
{
    const char *js = "{\"node\":\"!e5f6a7b8\",\"route\":\"direct\","
                     "\"config\":{\"numValves\":6,\"hbMinutes\":9}}";
    NodeConfigReq req;
    req.config.configEpoch = 100; // semeado (gerido) — deve sobreviver
    ParseResult r = parseNodeConfigReq(js, strlen(js), req);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_HEX32(0xe5f6a7b8, req.node);
    TEST_ASSERT_EQUAL_INT((int)SvcRoute::DIRECT, (int)req.route);
    TEST_ASSERT_EQUAL_UINT8(6, req.config.numValves);
    TEST_ASSERT_EQUAL_UINT32(100, req.config.configEpoch); // preservado
    NodeConfigReq bad;
    TEST_ASSERT_FALSE(parseNodeConfigReq("{\"node\":\"!1\",\"route\":\"x\",\"config\":{}}", 36, bad).ok);
}
```

- [ ] **Step 2: Rodar → RED.**

- [ ] **Step 3: Declarar no header**
```cpp
// ── Aba Rede — escrita de config (2 rotas §11.6) ─────────────────────────────
enum class SvcRoute : uint8_t { VIA_GATEWAY, DIRECT };
struct NodeConfigReq {
    uint32_t node = 0;
    SvcRoute route = SvcRoute::VIA_GATEWAY;
    IrrigationSettings config; // chamador semeia com o blob lido; parse reescreve os editáveis
};
// {node:"!hex", route:"gateway"|"direct", config:{…}}.
ParseResult parseNodeConfigReq(const char *json, size_t len, NodeConfigReq &out);
```

- [ ] **Step 4: Implementar no .cpp** (`#include <cstring>` já presente)
```cpp
ParseResult parseNodeConfigReq(const char *json, size_t len, NodeConfigReq &out)
{
    ParseResult r;
    Slice top{json, len};
    char nodeStr[16] = {0}, route[12] = {0};
    if (!IrrigationService::jsonStr(top, "node", nodeStr, sizeof nodeStr)) {
        r.fail("node ausente");
        return r;
    }
    out.node = IrrigationService::parseNodeHex(nodeStr);
    if (out.node == 0) {
        r.fail("node invalido");
        return r;
    }
    if (!IrrigationService::jsonStr(top, "route", route, sizeof route)) {
        r.fail("route ausente");
        return r;
    }
    if (strcmp(route, "gateway") == 0)
        out.route = SvcRoute::VIA_GATEWAY;
    else if (strcmp(route, "direct") == 0)
        out.route = SvcRoute::DIRECT;
    else {
        r.fail("route invalida");
        return r;
    }
    Slice cfg;
    if (!IrrigationService::jsonMember(json, len, "config", cfg)) {
        r.fail("config ausente");
        return r;
    }
    return parseStationConfig(cfg.p, cfg.n, out.config); // preserva os geridos do out.config
}
```

- [ ] **Step 5: Rodar → FILTERED (passa).**

- [ ] **Step 6: Commit**
```bash
git add src/modules/irrigation/ServicePortalApi.h src/modules/irrigation/ServicePortalApi.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): 8c parseNodeConfigReq (node+rota+config aninhada)"
```

---

## Task 7: `parseNodeAction` (pulso/zona/aprovar-par/resync)

**Files:**
- Modify: `src/modules/irrigation/ServicePortalApi.h`, `.cpp`
- Test: `test/test_service_portal/test_main.cpp`

**Interfaces:**
- Consumes: `IrrigationService::jsonStr`/`jsonInt`/`parseNodeHex`.
- Produces: `enum class SvcAction`, `struct NodeAction`, `ParseResult IrrigationWeb::parseNodeAction(const char *json, size_t len, NodeAction &out)`.

- [ ] **Step 1: Escrever o teste que falha** (`RUN_TEST(test_parseNodeAction);`)
```cpp
static void test_parseNodeAction()
{
    NodeAction a{};
    const char *pulse = "{\"node\":\"!11\",\"action\":\"pulse\",\"valveId\":2,\"durationS\":30}";
    ParseResult r = parseNodeAction(pulse, strlen(pulse), a);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT((int)SvcAction::PULSE, (int)a.action);
    TEST_ASSERT_EQUAL_UINT8(2, a.valveOrZoneId);
    TEST_ASSERT_EQUAL_UINT16(30, a.durationS);

    NodeAction z{};
    const char *zone = "{\"node\":\"!11\",\"action\":\"zone\",\"zoneId\":5,\"open\":1,\"durationS\":600}";
    TEST_ASSERT_TRUE(parseNodeAction(zone, strlen(zone), z).ok);
    TEST_ASSERT_EQUAL_INT((int)SvcAction::ZONE, (int)z.action);
    TEST_ASSERT_TRUE(z.open);

    NodeAction rs{};
    TEST_ASSERT_TRUE(parseNodeAction("{\"node\":\"!11\",\"action\":\"resync\"}", 33, rs).ok);
    TEST_ASSERT_EQUAL_INT((int)SvcAction::RESYNC, (int)rs.action);

    NodeAction bad{};
    TEST_ASSERT_FALSE(parseNodeAction("{\"node\":\"!11\",\"action\":\"nope\"}", 31, bad).ok);
}
```

- [ ] **Step 2: Rodar → RED.**

- [ ] **Step 3: Declarar no header**
```cpp
// ── Aba Rede — ações por nó ──────────────────────────────────────────────────
enum class SvcAction : uint8_t { NONE, PULSE, ZONE, APPROVE_PAIR, RESYNC };
struct NodeAction {
    uint32_t node = 0;
    SvcAction action = SvcAction::NONE;
    uint8_t valveOrZoneId = 0; // PULSE: valveId 0..7; ZONE: zoneId 1..255
    uint16_t durationS = 0;
    bool open = false; // ZONE: abrir(1)/fechar(0)
};
// {node:"!hex", action:"pulse"|"zone"|"approve_pair"|"resync", …}. "open" como 0/1.
ParseResult parseNodeAction(const char *json, size_t len, NodeAction &out);
```

- [ ] **Step 4: Implementar no .cpp**
```cpp
ParseResult parseNodeAction(const char *json, size_t len, NodeAction &out)
{
    ParseResult r;
    Slice top{json, len};
    char nodeStr[16] = {0}, act[16] = {0};
    if (!IrrigationService::jsonStr(top, "node", nodeStr, sizeof nodeStr)) {
        r.fail("node ausente");
        return r;
    }
    out.node = IrrigationService::parseNodeHex(nodeStr);
    if (out.node == 0) {
        r.fail("node invalido");
        return r;
    }
    if (!IrrigationService::jsonStr(top, "action", act, sizeof act)) {
        r.fail("action ausente");
        return r;
    }
    int64_t v;
    if (strcmp(act, "pulse") == 0) {
        out.action = SvcAction::PULSE;
        if (!IrrigationService::jsonInt(top, "valveId", v) || v < 0 || v > 7) {
            r.fail("valveId 0..7");
            return r;
        }
        out.valveOrZoneId = (uint8_t)v;
        if (!IrrigationService::jsonInt(top, "durationS", v) || v < 1 || v > 7200) {
            r.fail("durationS 1..7200");
            return r;
        }
        out.durationS = (uint16_t)v;
    } else if (strcmp(act, "zone") == 0) {
        out.action = SvcAction::ZONE;
        if (!IrrigationService::jsonInt(top, "zoneId", v) || v < 1 || v > 255) {
            r.fail("zoneId 1..255");
            return r;
        }
        out.valveOrZoneId = (uint8_t)v;
        if (!IrrigationService::jsonInt(top, "open", v)) {
            r.fail("open ausente");
            return r;
        }
        out.open = v != 0;
        if (out.open) {
            if (!IrrigationService::jsonInt(top, "durationS", v) || v < 1 || v > 7200) {
                r.fail("durationS 1..7200");
                return r;
            }
            out.durationS = (uint16_t)v;
        } else {
            out.durationS = 0;
        }
    } else if (strcmp(act, "approve_pair") == 0) {
        out.action = SvcAction::APPROVE_PAIR;
    } else if (strcmp(act, "resync") == 0) {
        out.action = SvcAction::RESYNC;
    } else {
        r.fail("action desconhecida");
        return r;
    }
    return r;
}
```

- [ ] **Step 5: Rodar → FILTERED (passa).**

- [ ] **Step 6: Commit**
```bash
git add src/modules/irrigation/ServicePortalApi.h src/modules/irrigation/ServicePortalApi.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): 8c parseNodeAction (pulso/zona/aprovar-par/resync)"
```

---

## Task 8: `buildServiceLog` + `IServiceLogReader` (aba Log — pura)

**Files:**
- Modify: `src/modules/irrigation/ServicePortalApi.h`, `.cpp`
- Test: `test/test_service_portal/test_main.cpp`

**Interfaces:**
- Consumes: `IrrigationWeb::JsonWriter` (usa `raw()` p/ inserir linhas já-JSON).
- Produces: `struct IServiceLogReader { virtual void forEachLine(size_t maxLines, void *ctx, void (*cb)(void *, const char *line)) = 0; virtual ~IServiceLogReader() = default; }`, `size_t IrrigationWeb::buildServiceLog(IServiceLogReader &reader, size_t maxLines, char *buf, size_t cap)`.

- [ ] **Step 1: Escrever o teste que falha** (com duplê RAM inline; `RUN_TEST(test_buildServiceLog);`)
```cpp
struct RamLogReader : IServiceLogReader {
    const char *lines[8];
    size_t n = 0;
    void forEachLine(size_t maxLines, void *ctx, void (*cb)(void *, const char *)) override
    {
        size_t start = (n > maxLines) ? n - maxLines : 0;
        for (size_t i = start; i < n; i++)
            cb(ctx, lines[i]);
    }
};

static void test_buildServiceLog()
{
    RamLogReader rd;
    rd.lines[0] = "{\"up\":10,\"ev\":\"select\"}";
    rd.lines[1] = "{\"up\":20,\"ev\":\"scan\"}";
    rd.n = 2;
    char buf[512];
    size_t n = buildServiceLog(rd, 100, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"log\":["));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ev\":\"select\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ev\":\"scan\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "},{")); // duas entradas separadas por vírgula
}
```

- [ ] **Step 2: Rodar → RED.**

- [ ] **Step 3: Declarar no header**
```cpp
// ── Aba Log ──────────────────────────────────────────────────────────────────
// Leitor injetável do servico.jsonl: entrega cada linha (já-JSON, NUL-terminada),
// da mais antiga p/ mais nova, limitado às últimas maxLines. Impl LittleFS é glue.
struct IServiceLogReader {
    virtual void forEachLine(size_t maxLines, void *ctx, void (*cb)(void *, const char *line)) = 0;
    virtual ~IServiceLogReader() = default;
};
// {"log":[ <linha>, <linha>, … ]}.
size_t buildServiceLog(IServiceLogReader &reader, size_t maxLines, char *buf, size_t cap);
```

- [ ] **Step 4: Implementar no .cpp**
```cpp
namespace
{
struct LogBuildCtx {
    JsonWriter *w;
};
void logLineCb(void *ctx, const char *line)
{
    static_cast<LogBuildCtx *>(ctx)->w->raw(line); // cada linha já é um objeto JSON
}
} // namespace

size_t buildServiceLog(IServiceLogReader &reader, size_t maxLines, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("log");
    w.beginArray();
    LogBuildCtx c{&w};
    reader.forEachLine(maxLines, &c, logLineCb);
    w.endArray();
    w.endObject();
    return w.done();
}
```

- [ ] **Step 5: Rodar → FILTERED (passa). Depois rodar a suíte completa e confirmar GREEN.**

Run: `./bin/run-tests.sh` (via Docker, run completo)
Expected: `RESULT: GREEN 60/60 suites passed [canonical: 60/60]` (exit 0).

- [ ] **Step 6: Commit**
```bash
git add src/modules/irrigation/ServicePortalApi.h src/modules/irrigation/ServicePortalApi.cpp test/test_service_portal/test_main.cpp
git commit -m "feat(irrigation): 8c buildServiceLog + IServiceLogReader (aba Log pura)"
```

---

## Task 9: Writer/reader do `servico.jsonl` (glue de store + ServiceController)

**Files:**
- Modify: `src/modules/irrigation/IProfileStore.h` — métodos de log + `IServiceLogReader` no store.
- Modify: `src/modules/irrigation/ServiceController.h`, `.cpp` — `logService(...)`.
- Modify: `src/modules/irrigation/LittleFsProfileStore.h`, `.cpp` — impl append/tail.
- Modify: `test/.../support/RamProfileStore.h` — append/tail em RAM (duplê).

**Interfaces:**
- Consumes: `IProfileStore` (8b), `IrrigationWeb::JsonWriter`.
- Produces: `IProfileStore::appendLog(const char *line)` + `IProfileStore::logReader() → IrrigationWeb::IServiceLogReader&`; `void ServiceController::logService(const char *ev, uint32_t node, uint32_t gwTs)`.

> **Verificação:** glue não é TDD nativo (toca FSCom); valida por **compilação nativa** — o duplê RAM do store + `buildServiceLog` já são exercidos pela suite. Rodar `./bin/run-tests.sh` e confirmar GREEN (a suíte compila `ServiceController.cpp`, `RamProfileStore.h`).

- [ ] **Step 1: Estender `IProfileStore` (interface pura, `IProfileStore.h`)**

Adicionar ao `struct IProfileStore` (após os métodos existentes) — declarar o include do reader:
```cpp
#include "modules/irrigation/ServicePortalApi.h" // IrrigationWeb::IServiceLogReader
// …
    // Log de serviço (§11.2 /log/servico.jsonl). Append de uma linha JSONL; reader p/ a aba Log.
    virtual bool appendLog(const char *line) = 0;
    virtual IrrigationWeb::IServiceLogReader &logReader() = 0;
```

- [ ] **Step 2: Implementar no duplê RAM (`support/RamProfileStore.h`)**

Adicionar um buffer circular simples + reader interno:
```cpp
// dentro de RamProfileStore:
    static constexpr size_t LOG_MAX = 64;
    char logLines[LOG_MAX][160];
    size_t logCount = 0;
    struct RamReader : IrrigationWeb::IServiceLogReader {
        RamProfileStore *s;
        void forEachLine(size_t maxLines, void *ctx, void (*cb)(void *, const char *)) override
        {
            size_t start = (s->logCount > maxLines) ? s->logCount - maxLines : 0;
            for (size_t i = start; i < s->logCount; i++)
                cb(ctx, s->logLines[i % LOG_MAX]);
        }
    } reader{this};
    bool appendLog(const char *line) override
    {
        size_t slot = logCount % LOG_MAX;
        strncpy(logLines[slot], line, sizeof(logLines[slot]) - 1);
        logLines[slot][sizeof(logLines[slot]) - 1] = 0;
        logCount++;
        return true;
    }
    IrrigationWeb::IServiceLogReader &logReader() override { return reader; }
```
(Se `logCount` exceder `LOG_MAX`, `start` usa a contagem total mas o `% LOG_MAX` lê as últimas — mantém o tail correto para `maxLines ≤ LOG_MAX`.)

- [ ] **Step 3: Impl LittleFS (`LittleFsProfileStore.cpp`)** — append com FSCom, tail lendo as últimas linhas; no-op sem FSCom.
```cpp
bool LittleFsProfileStore::appendLog(const char *line)
{
#ifdef FSCom
    auto f = FSCom.open("/clientes/log/servico.jsonl", FILE_O_APPEND);
    if (!f)
        return false;
    f.write((const uint8_t *)line, strlen(line));
    f.write((const uint8_t *)"\n", 1);
    f.close();
    return true;
#else
    (void)line;
    return false;
#endif
}
```
O `logReader()` devolve um membro `LittleFsLogReader` cujo `forEachLine` lê o arquivo e entrega as últimas `maxLines` (buffer de linha fixo ~160 B; sem FSCom → não chama cb). Declarar o membro e a classe no `.h` espelhando o padrão do duplê RAM.

- [ ] **Step 4: `ServiceController::logService` (`ServiceController.h`/`.cpp`)**
```cpp
// ServiceController.h (público):
    void logService(const char *ev, uint32_t node, uint32_t gwTs);
```
```cpp
// ServiceController.cpp:
#include "modules/irrigation/IrrigationWebApi.h"
void ServiceController::logService(const char *ev, uint32_t node, uint32_t gwTs)
{
    char line[160];
    IrrigationWeb::JsonWriter w(line, sizeof line);
    w.beginObject();
    w.keyNum("up", (int64_t)(millis() / 1000)); // uptime; device sem RTC
    if (gwTs)
        w.keyNum("ts", (int64_t)gwTs); // timestamp adotado do gateway, se houver
    w.keyStr("ev", ev);
    if (node)
        w.keyNum("node", (int64_t)node);
    w.endObject();
    if (w.done())
        vault.getStore().appendLog(line);
}
```
> Requer expor o store no vault: adicionar `IProfileStore &getStore() { return store; }` em `ServiceVault` (`ServiceVault.h`). `millis()` vem de `Arduino.h` (nativo: shim do portduino — já usado por outros glues).

- [ ] **Step 5: Rodar a suíte completa → GREEN** (`./bin/run-tests.sh`; confirma que store/controller/RamProfileStore compilam e `test_service_portal`+`test_service_*` seguem verdes).

- [ ] **Step 6: Commit**
```bash
git add src/modules/irrigation/IProfileStore.h src/modules/irrigation/ServiceController.h \
        src/modules/irrigation/ServiceController.cpp src/modules/irrigation/ServiceVault.h \
        src/modules/irrigation/LittleFsProfileStore.h src/modules/irrigation/LittleFsProfileStore.cpp \
        test/test_service_controller test/test_service_vault
git commit -m "feat(irrigation): 8c writer/reader do servico.jsonl (store + ServiceController.logService)"
```

---

## Task 10: Accessors `svcPortal*` — clientes/select/scan (glue do módulo)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` — decls públicas.
- Modify: `src/modules/irrigation/IrrigationModule.cpp` — impl.

**Interfaces:**
- Consumes: `ServiceController` (membro `svc`), `applyRetune` (existe, 1157), `svcEmitProbe` (existe, 1183), `IrrigationWeb::buildClientList`/`buildScanResults`/`parseSelect`.
- Produces: `bool svcIsService()`, `size_t svcPortalListClients(char*, size_t)`, `bool svcPortalSelect(const char *id)`, `void svcPortalStartScan()`, `size_t svcPortalScanResults(char*, size_t)`.

> **Verificação:** glue — valida por compilação nativa (`svc`, `applyRetune`, `svcEmitProbe` já compilam no nativo). Rodar `./bin/run-tests.sh` → GREEN (módulo compila).

- [ ] **Step 1: Declarar no header** (`IrrigationModule.h`, junto dos outros `portal*`/`svc*`)
```cpp
    // Fase 8c — portal do device SERVICO (§11.8).
    bool svcIsService() const;
    size_t svcPortalListClients(char *buf, size_t cap);
    bool svcPortalSelect(const char *id); // dispara applyRetune (re-tune + reboot)
    void svcPortalStartScan();
    size_t svcPortalScanResults(char *buf, size_t cap);
```

- [ ] **Step 2: Implementar no .cpp** (após `svcExportToConsole`, ~1232)
```cpp
bool IrrigationModule::svcIsService() const
{
    return settings.role == (uint8_t)IrrigationRole::SERVICO;
}

size_t IrrigationModule::svcPortalListClients(char *buf, size_t cap)
{
    if (!svc)
        return 0;
    IrrigationService::LightProfile cs[16];
    size_t n = svc->getVault().listClients(cs, 16);
    char active[32] = {0};
    svc->getVault().activeId(active, sizeof active);
    return IrrigationWeb::buildClientList(cs, n, active, buf, cap);
}

bool IrrigationModule::svcPortalSelect(const char *id)
{
    if (!svc)
        return false;
    RetunePlan probe;
    if (!svc->planRetune(id, probe)) // valida existência + PSK antes de reiniciar
        return false;
    svc->logService("select", 0, gwTimeAdopted());
    applyRetune(id); // persiste ativo + reboot em 3 s (§11.3)
    return true;
}

void IrrigationModule::svcPortalStartScan()
{
    if (!svc)
        return;
    svc->logService("scan", 0, gwTimeAdopted());
    svcEmitProbe(); // limpa scan + broadcast PROBE (8b)
}

size_t IrrigationModule::svcPortalScanResults(char *buf, size_t cap)
{
    if (!svc)
        return 0;
    return IrrigationWeb::buildScanResults(svc->scanResults(), buf, cap);
}
```
> `gwTimeAdopted()` = helper que devolve o último timestamp adotado do gateway (0 se nenhum). Se ainda não existir, adicionar um getter simples que retorna `0` por ora (device sem RTC; ts é best-effort — preencher quando o intake de heartbeat/ACK do gateway registrar tempo). Declarar `uint32_t gwTimeAdopted() const { return 0; }` no header como stub honesto (marcar TODO de banca).

- [ ] **Step 3: Rodar a suíte completa → GREEN** (`./bin/run-tests.sh`).

- [ ] **Step 4: Commit**
```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): 8c svcPortal accessors — clientes/select/scan"
```

---

## Task 11: Accessors `svcPortal*` — read/write config + ação por nó (glue de rádio)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `.cpp`

**Interfaces:**
- Consumes: `decideConfigRoute`/`nextSeq` (8b), `encodeSetConfig`+`SetConfig`+`FRAG_DATA_MAX`+`crc32` (padrão em `IrrigationModule.cpp:2585`), `encodeGetConfig` (2618), `svcSendResyncRequest` (existe, 1204), `setServiceFlag`, `allocDataPacket`, `service->sendToMesh`, `migrateIrrigationSettings`, `FragmentReassembler`.
- Produces: `bool svcPortalReadConfig(uint32_t node)`, `bool svcPortalConfigReady(uint32_t node)`, `size_t svcPortalGetReadConfig(char *buf, size_t cap)`, `bool svcPortalWriteConfig(const IrrigationWeb::NodeConfigReq &req)`, `bool svcPortalNodeAction(const IrrigationWeb::NodeAction &a)`.

> **Verificação:** glue de rádio — **não native-testável**; valida por compilação nativa (`./bin/run-tests.sh` GREEN) + **banca 2+ nós**. Os globais/encoders usados já existem no build nativo.

- [ ] **Step 1: Declarar no header**
```cpp
    // Leitura de config: envia GET_CONFIG direto; o reply (SET_CONFIG-frag) é remontado no intake.
    bool svcPortalReadConfig(uint32_t node);
    bool svcPortalConfigReady(uint32_t node);          // reply completo já chegou?
    size_t svcPortalGetReadConfig(char *buf, size_t cap); // buildStationConfig do blob lido
    bool svcPortalWriteConfig(const IrrigationWeb::NodeConfigReq &req);
    bool svcPortalNodeAction(const IrrigationWeb::NodeAction &a);
```
Adicionar membros privados de intake do GET_CONFIG:
```cpp
    uint32_t svcReadNode = 0;              // nó cuja leitura está pendente/pronta
    bool svcReadReady = false;
    IrrigationSettings svcReadBlob;        // blob remontado do reply
    FragmentReassembler svcReasm;          // remontagem do SET_CONFIG-frag de resposta
```

- [ ] **Step 2: Implementar leitura (envio GET_CONFIG)**
```cpp
bool IrrigationModule::svcPortalReadConfig(uint32_t node)
{
    if (!svc || !node)
        return false;
    svcReadNode = node;
    svcReadReady = false;
    svcReasm.reset();
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = node;
    p->decoded.payload.size =
        (uint16_t)encodeGetConfig(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return false;
    }
    setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    svc->logService("get_config", node, gwTimeAdopted());
    return true;
}

bool IrrigationModule::svcPortalConfigReady(uint32_t node) { return svcReadReady && svcReadNode == node; }

size_t IrrigationModule::svcPortalGetReadConfig(char *buf, size_t cap)
{
    if (!svcReadReady)
        return 0;
    return IrrigationWeb::buildStationConfig(svcReadBlob, buf, cap);
}
```

- [ ] **Step 3: Intake do reply no handler `MSG_SET_CONFIG`**

No dispatch existente (`IrrigationModule.cpp:325`, `case MSG_SET_CONFIG`), quando `svcIsService()` e o `from == svcReadNode`, alimentar `svcReasm` com o fragmento; ao completar (CRC ok), `migrateIrrigationSettings(reasm.blob(), reasm.blobLen(), svcReadBlob)` e `svcReadReady = true`. Espelhar a remontagem já usada no gateway (`handleGwSetConfig`, ~2750). Estrutura:
```cpp
    case MSG_SET_CONFIG:
        if (svcIsService() && mp.from == svcReadNode) {
            if (svcReasm.feed(/* SetConfig decodificado */) && svcReasm.complete()) {
                IrrigationSettings blob;
                if (migrateIrrigationSettings(svcReasm.blob(), svcReasm.blobLen(), blob)) {
                    svcReadBlob = blob;
                    svcReadReady = true;
                }
            }
            return;
        }
        // … caminho GATEWAY existente …
```
> Casar a API real do `FragmentReassembler` (ver `handleGwSetConfig`); manter o caminho GATEWAY intacto.

- [ ] **Step 4: Implementar escrita (2 rotas §11.6)**
```cpp
bool IrrigationModule::svcPortalWriteConfig(const IrrigationWeb::NodeConfigReq &req)
{
    if (!svc)
        return false;
    uint32_t node = req.node;
    // Serializa o blob v5 do editor p/ os fragmentos SET_CONFIG.
    uint8_t blob[sizeof(IrrigationSettings)];
    memcpy(blob, &req.config, sizeof blob);
    uint16_t totalLen = sizeof blob;
    uint32_t crc = crc32(blob, totalLen);

    bool viaGw = req.route == IrrigationWeb::SvcRoute::VIA_GATEWAY;
    uint32_t target = node;   // DIRECT: escreve na estação
    uint32_t epoch = req.config.configEpoch;
    if (!viaGw) {
        RouteDecision d = decideConfigRoute(false, req.config.configEpoch); // DIRECT → epoch+1
        epoch = d.epochToWrite;
        ((IrrigationSettings *)blob)->configEpoch = epoch; // grava epoch+1 no blob enviado
        crc = crc32(blob, totalLen);
    } else {
        // VIA_GATEWAY: envia ao gateway do cliente; ele incrementa o epoch e propaga (§5.4).
        target = 0; // resolvido abaixo pelo gateway do perfil ativo
    }
    // Envio fragmentado (mesmo padrão de IrrigationModule.cpp:2591).
    uint8_t fragCount = (uint8_t)((totalLen + FRAG_DATA_MAX - 1) / FRAG_DATA_MAX);
    uint32_t dest = viaGw ? svcActiveGateway() : target;
    if (viaGw && dest == 0)
        return false; // sem gateway conhecido → orienta usuário a usar rota direta
    for (uint8_t i = 0; i < fragCount; i++) {
        SetConfig sc = {};
        sc.epoch = epoch;
        sc.crc = crc;
        sc.totalLen = totalLen;
        sc.fragIndex = i;
        sc.fragCount = fragCount;
        uint16_t off = (uint16_t)i * FRAG_DATA_MAX;
        sc.fragLen = (uint8_t)((totalLen - off > FRAG_DATA_MAX) ? FRAG_DATA_MAX : (uint8_t)(totalLen - off));
        sc.frag = blob + off;
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = dest;
        p->decoded.payload.size =
            (uint16_t)encodeSetConfig(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, sc);
        if (!p->decoded.payload.size) {
            packetPool.release(p);
            return false;
        }
        setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    }
    svc->logService(viaGw ? "set_config_gw" : "set_config_direct", node, gwTimeAdopted());
    return true;
}
```
> `svcActiveGateway()` = getter do gateway do perfil ativo (do `LightProfile.gateway` do cliente selecionado no vault). Se ainda não existir, adicionar consultando `listClients` + `activeId`. `FRAG_DATA_MAX`, `SetConfig`, `encodeSetConfig`, `crc32` já em uso no arquivo.

- [ ] **Step 5: Implementar ação por nó**
```cpp
bool IrrigationModule::svcPortalNodeAction(const IrrigationWeb::NodeAction &a)
{
    if (!svc || !a.node)
        return false;
    switch (a.action) {
    case IrrigationWeb::SvcAction::RESYNC:
        svcSendResyncRequest(a.node); // 8b (já carimba FLAG_FROM_SERVICE)
        svc->logService("resync", a.node, gwTimeAdopted());
        return true;
    case IrrigationWeb::SvcAction::PULSE:
    case IrrigationWeb::SvcAction::ZONE:
    case IrrigationWeb::SvcAction::APPROVE_PAIR:
        // Reusa os emissores de comando remoto/pareamento (mesmo padrão allocDataPacket/encode/
        // setServiceFlag/sendToMesh dos executores 8b). Casar com os encoders existentes:
        //  - PULSE  → comando de pulso p/ a estação (encoder de pulso/RemoteCmd).
        //  - ZONE   → RemoteCmd {zoneId, action=open/close, durationS} (ver handleRemoteCmd:2646).
        //  - APPROVE_PAIR → PAIR_GRANT p/ a.node (Fase 3; ver case MSG_PAIR_GRANT:338).
        return svcSendNodeCommand(a); // helper novo abaixo
    default:
        return false;
    }
}
```
> Implementar `svcSendNodeCommand(const IrrigationWeb::NodeAction&)` como um único helper que monta o pacote conforme a ação, reutilizando os encoders confirmados no arquivo (`RemoteCmd` para zona, o encoder de pulso e o de PAIR_GRANT), sempre `setServiceFlag` + `svc->logService`. **Validação em banca** — encoders de comando remoto não são native-testáveis.

- [ ] **Step 6: Rodar a suíte completa → GREEN + Commit**
```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): 8c svcPortal read/write config (2 rotas) + ação por nó"
```

---

## Task 12: Endpoints ESP32 `/api/portal/service/*` (clientes/scan/config/ação/log)

**Files:**
- Create: `src/modules/irrigation/ServicePortalEndpoints.h`, `.cpp`
- Modify: `variants/native/portduino.ini` — excluir `.cpp` do nativo.
- Modify: `src/mesh/http/ContentHandler.cpp` — registrar (2 sítios).

**Interfaces:**
- Consumes: `IrrigationModule::svcPortal*` (Tasks 10–11), `IrrigationWeb::parseSelect`/`parseNodeConfigReq`/`parseNodeAction`, `httpsserver`.
- Produces: `void registerIrrigationServicePortalHandlers(httpsserver::HTTPServer *server)`.

> **Verificação:** só-ESP32 — **excluído do nativo**; garantir que o nativo segue GREEN (exclusão correta) + **CI compila ESP32**. Sem TDD nativo.

- [ ] **Step 1: Header** (`ServicePortalEndpoints.h`) — espelhar `IrrigationPortalEndpoints.h`:
```cpp
#pragma once
#if !MESHTASTIC_EXCLUDE_WEBSERVER
namespace httpsserver { class HTTPServer; }
// Registra as rotas /api/portal/service/* (Fase 8c, gated role==SERVICO). Chamada por ContentHandler.
void registerIrrigationServicePortalHandlers(httpsserver::HTTPServer *server);
#endif
```

- [ ] **Step 2: .cpp com handlers** (`ServicePortalEndpoints.cpp`) — espelhar `IrrigationPortalEndpoints.cpp` (helpers `sendJson`/`sendParseErrors`/`readBody`; guarda de role em cada handler):
```cpp
#include "modules/irrigation/ServicePortalEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/ServicePortalApi.h"
#include <Arduino.h>
#include <cstdlib>
#undef str
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <ResourceNode.hpp>

using namespace httpsserver;
using namespace IrrigationWeb;

// sendJson / sendParseErrors / readBody: copiar de IrrigationPortalEndpoints.cpp.

static bool svcGuard(HTTPResponse *res)
{
    if (!irrigationModule || !irrigationModule->svcIsService()) {
        res->setStatusCode(404);
        return false;
    }
    return true;
}

static void hSvcClients(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res))
        return;
    char buf[2560];
    size_t n = irrigationModule->svcPortalListClients(buf, sizeof buf);
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

static void hSvcSelect(HTTPRequest *req, HTTPResponse *res)
{
    if (!svcGuard(res))
        return;
    char body[64];
    size_t nb = readBody(req, body, sizeof body);
    char id[32] = {0};
    ParseResult pr = parseSelect(body, nb, id, sizeof id);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->svcPortalSelect(id)) {
        sendJson(res, "{\"errors\":[\"cliente inexistente ou PSK invalida\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true,\"rebooting\":true}", 202); // re-tune + reboot em 3 s
}

static void hSvcScanStart(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res)) return;
    irrigationModule->svcPortalStartScan();
    sendJson(res, "{\"ok\":true}");
}

static void hSvcScanGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res)) return;
    char buf[4096];
    size_t n = irrigationModule->svcPortalScanResults(buf, sizeof buf);
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

static void hSvcConfigGet(HTTPRequest *req, HTTPResponse *res) // GET ?node=!hex
{
    if (!svcGuard(res)) return;
    std::string q = req->getParams()->getQueryParameter("node") ? ... ; // ver nota
    uint32_t node = IrrigationService::parseNodeHex(q.c_str());
    // 1ª chamada dispara a leitura; o cliente faz poll até ready.
    if (!irrigationModule->svcPortalConfigReady(node)) {
        irrigationModule->svcPortalReadConfig(node);
        sendJson(res, "{\"pending\":true}", 202);
        return;
    }
    char buf[3072];
    size_t n = irrigationModule->svcPortalGetReadConfig(buf, sizeof buf);
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

static void hSvcConfigSet(HTTPRequest *req, HTTPResponse *res)
{
    if (!svcGuard(res)) return;
    char *body = (char *)malloc(3072);
    if (!body) { res->setStatusCode(500); return; }
    size_t nb = readBody(req, body, 3072);
    NodeConfigReq cr;
    // Semear os campos geridos com o último blob lido do mesmo nó (se houver):
    // (o handler pode reusar svcPortalGetReadConfig internamente; simplificação: parse já preserva
    //  defaults e o módulo aplica epoch conforme a rota).
    ParseResult pr = parseNodeConfigReq(body, nb, cr);
    if (!pr.ok) { free(body); sendParseErrors(res, pr); return; }
    bool ok = irrigationModule->svcPortalWriteConfig(cr);
    free(body);
    sendJson(res, ok ? "{\"ok\":true}" : "{\"errors\":[\"escrita rejeitada (sem gateway p/ rota via-gateway?)\"]}",
             ok ? 200 : 400);
}

static void hSvcAction(HTTPRequest *req, HTTPResponse *res)
{
    if (!svcGuard(res)) return;
    char body[128];
    size_t nb = readBody(req, body, sizeof body);
    NodeAction a;
    ParseResult pr = parseNodeAction(body, nb, a);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    bool ok = irrigationModule->svcPortalNodeAction(a);
    sendJson(res, ok ? "{\"ok\":true}" : "{\"errors\":[\"acao rejeitada\"]}", ok ? 200 : 400);
}

static void hSvcLog(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res)) return;
    const size_t cap = 10240;
    char *buf = (char *)malloc(cap);
    if (!buf) { res->setStatusCode(500); return; }
    size_t n = irrigationModule->svcPortalBuildLog(buf, cap); // helper: buildServiceLog(store.logReader(),100,…)
    if (!n) { free(buf); res->setStatusCode(500); return; }
    sendJson(res, buf);
    free(buf);
}

void registerIrrigationServicePortalHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/portal/service/clients", "GET", &hSvcClients));
    server->registerNode(new ResourceNode("/api/portal/service/select", "POST", &hSvcSelect));
    server->registerNode(new ResourceNode("/api/portal/service/scan", "POST", &hSvcScanStart));
    server->registerNode(new ResourceNode("/api/portal/service/scan", "GET", &hSvcScanGet));
    server->registerNode(new ResourceNode("/api/portal/service/node/config", "GET", &hSvcConfigGet));
    server->registerNode(new ResourceNode("/api/portal/service/node/config", "POST", &hSvcConfigSet));
    server->registerNode(new ResourceNode("/api/portal/service/node/action", "POST", &hSvcAction));
    server->registerNode(new ResourceNode("/api/portal/service/log", "GET", &hSvcLog));
}

#endif
```
> **Notas:** (a) casar `getQueryParameter` com a API real do `httpsserver` usada no repo (ver como outros endpoints leem query; se inexistente, aceitar `node` via corpo POST num endpoint `node/config/read`). (b) Adicionar o helper fino `size_t IrrigationModule::svcPortalBuildLog(char*, size_t)` que chama `IrrigationWeb::buildServiceLog(svcStore->logReader(), 100, buf, cap)`. (c) `svcPortalScanResults`/`buildLog` buffers no heap se necessário (padrão `hLog`).

- [ ] **Step 3: Excluir do build nativo** (`variants/native/portduino.ini`, após a linha 18):
```ini
  -<modules/irrigation/ServicePortalEndpoints.cpp>  ; cola HTTP só-ESP32 (esp32_https_server); nativo exclui
```

- [ ] **Step 4: Registrar no `ContentHandler.cpp`** — incluir o header e chamar nos 2 sítios (após `registerIrrigationPortalHandlers`, linhas 123 e 149):
```cpp
#include "modules/irrigation/ServicePortalEndpoints.h" // (junto dos outros includes)
// …após registerIrrigationPortalHandlers(secureServer);
    registerIrrigationServicePortalHandlers(secureServer);
// …após registerIrrigationPortalHandlers(insecureServer);
    registerIrrigationServicePortalHandlers(insecureServer);
```

- [ ] **Step 5: Rodar a suíte nativa → GREEN** (`./bin/run-tests.sh`) — confirma que a exclusão do `.cpp` está correta (nativo NÃO tenta compilar `<HTTPRequest.hpp>`). CI do fork compila o ESP32.

- [ ] **Step 6: Commit**
```bash
git add src/modules/irrigation/ServicePortalEndpoints.h src/modules/irrigation/ServicePortalEndpoints.cpp \
        variants/native/portduino.ini src/mesh/http/ContentHandler.cpp src/modules/irrigation/IrrigationModule.h \
        src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): 8c endpoints /api/portal/service/* (clientes/scan/config/ação/log)"
```

---

## Task 13: Endpoints import/export via portal (§11.7) + staging streaming

**Files:**
- Modify: `src/modules/irrigation/ServicePortalEndpoints.cpp` — `hSvcExport`, `hSvcImport`.
- Modify: `src/modules/irrigation/IrrigationModule.h`, `.cpp` — `svcPortalExport`, `svcPortalImport`.

**Interfaces:**
- Consumes: `ServiceVault::exportEnvelope`/`importEnvelope` (8b).
- Produces: `size_t IrrigationModule::svcPortalExport(char *buf, size_t cap)`, `bool IrrigationModule::svcPortalImport(const char *stagingPath, bool replace, char *err, size_t errCap)`.

> **Verificação:** só-ESP32 — compilação nativa GREEN (excluído) + banca. Export plaintext (§ modelo de ameaça).

- [ ] **Step 1: Accessors no módulo** (`IrrigationModule.cpp`)
```cpp
size_t IrrigationModule::svcPortalExport(char *buf, size_t cap)
{
    if (!svc)
        return 0;
    return svc->getVault().exportEnvelope(buf, cap); // envelope multi-cliente (plaintext, §11.7)
}

bool IrrigationModule::svcPortalImport(const char *json, size_t n, bool replace, char *err, size_t errCap)
{
    if (!svc)
        return false;
    bool ok = svc->getVault().importEnvelope(json, n, replace, err, errCap); // valida em staging + merge por id
    if (ok)
        svc->logService(replace ? "import_replace" : "import_merge", 0, gwTimeAdopted());
    return ok;
}
```
> Declarar ambos no header. Assinatura de import usa buffer (o vault valida internamente antes de aplicar). O **staging em arquivo** é feito no handler (Step 2): o corpo POST é escrito num arquivo tmp em FSCom, relido e passado a `importEnvelope` — evita manter o corpo inteiro em pilha e casa o "validado em área temporária antes de substituir" (§11.7).

- [ ] **Step 2: Handlers export/import** (`ServicePortalEndpoints.cpp`)
```cpp
static void hSvcExport(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res)) return;
    const size_t cap = 32768; // teto do envelope multi-cliente (documentado); overflow → 500
    char *buf = (char *)malloc(cap);
    if (!buf) { res->setStatusCode(500); return; }
    size_t n = irrigationModule->svcPortalExport(buf, cap);
    if (!n) { free(buf); sendJson(res, "{\"errors\":[\"export overflow (>32KB)\"]}", 500); return; }
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Content-Disposition", "attachment; filename=\"irrig-vault.json\"");
    res->print(buf);
    free(buf);
}

static void hSvcImport(HTTPRequest *req, HTTPResponse *res) // POST ?replace=1
{
    if (!svcGuard(res)) return;
    // Stream do corpo → arquivo de staging em FSCom (não segura o envelope inteiro na pilha).
    const char *staging = "/clientes/import.tmp";
#ifdef FSCom
    auto f = FSCom.open(staging, FILE_O_WRITE);
    if (!f) { res->setStatusCode(500); return; }
    uint8_t chunk[512];
    size_t total = 0;
    for (;;) {
        size_t r = req->readBytes(chunk, sizeof chunk);
        if (r == 0) break;
        f.write(chunk, r);
        total += r;
        if (total > 65536) { f.close(); FSCom.remove(staging); sendJson(res, "{\"errors\":[\"import >64KB\"]}", 413); return; }
    }
    f.close();
    // Relê o staging p/ RAM (≤64 KB) e valida+aplica via engine do cofre.
    char *buf = (char *)malloc(total + 1);
    if (!buf) { FSCom.remove(staging); res->setStatusCode(500); return; }
    auto rf = FSCom.open(staging, FILE_O_READ);
    size_t got = rf ? rf.readBytes(buf, total) : 0;
    if (rf) rf.close();
    buf[got] = 0;
    FSCom.remove(staging);
    bool replace = req->getParams() && req->getParams()->getQueryParameter("replace"); // casar API real
    char err[64] = {0};
    bool ok = irrigationModule->svcPortalImport(buf, got, replace, err, sizeof err);
    free(buf);
    if (!ok) { char m[128]; snprintf(m, sizeof m, "{\"errors\":[\"%s\"]}", err[0] ? err : "import invalido"); sendJson(res, m, 400); return; }
    sendJson(res, "{\"ok\":true}");
#else
    res->setStatusCode(500);
#endif
}
```
E registrar em `registerIrrigationServicePortalHandlers`:
```cpp
    server->registerNode(new ResourceNode("/api/portal/service/export", "GET", &hSvcExport));
    server->registerNode(new ResourceNode("/api/portal/service/import", "POST", &hSvcImport));
```
> Casar `getQueryParameter`/`readBytes`/`FILE_O_*` com as APIs reais do repo (ver `IrrigationPortalEndpoints.cpp` p/ `readBytes` e `LittleFsProfileStore.cpp` p/ os modos FSCom). Teto de import 64 KB documentado.

- [ ] **Step 2b: Rodar a suíte nativa → GREEN** (exclusão do `.cpp` cobre o nativo).

- [ ] **Step 3: Commit**
```bash
git add src/modules/irrigation/ServicePortalEndpoints.cpp src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): 8c import/export via portal (§11.7) com staging em FSCom"
```

---

## Task 14: Frontend — abas Clientes/Rede/Log (role==SERVICO)

**Files:**
- Modify: `data/irrigacao/portal/index.html`, `data/irrigacao/portal/app.js`

**Interfaces:**
- Consumes: `/api/portal/node` (devolve `role`), `/api/portal/service/*`.

> **Verificação:** estático — sem teste nativo; validação por **banca** (carregar o portal num device SERVICO). Confere só que os arquivos existem e o JS não quebra o bundle atual.

- [ ] **Step 1: Tabs condicionais no `index.html`** — adicionar 3 seções `#tab-clientes`, `#tab-rede`, `#tab-log` (ocultas por default) + botões de aba; reusa o CSS de `data/irrigacao/style.css`.

- [ ] **Step 2: Lógica no `app.js`** — no bootstrap, após ler `/api/portal/node`, se `role===3` (SERVICO): mostrar as 3 abas de serviço e esconder as abas de node/estação; senão manter o comportamento atual (5b). Funções:
```js
// Clientes
async function loadClients(){ const d = await getJSON('/api/portal/service/clients'); renderClients(d.clients); }
async function selectClient(id){ await postJSON('/api/portal/service/select',{id});
  showBanner('Re-tunando no canal do cliente — o device vai reiniciar (~3 s). Reconecte ao portal.'); }
async function startScan(){ await postJSON('/api/portal/service/scan',{}); pollScan(); }
async function pollScan(){ const d = await getJSON('/api/portal/service/scan'); renderNodes(d.nodes); }
function downloadExport(){ location.href='/api/portal/service/export'; }
async function importVault(file, replace){ await fetch('/api/portal/service/import'+(replace?'?replace=1':''),
  {method:'POST', body:file}); }
// Rede — editor de config
async function readConfig(node){ // poll até !pending
  let d; do { d = await getJSON('/api/portal/service/node/config?node='+node); await sleep(500); } while(d.pending);
  fillConfigEditor(d); }
async function writeConfig(node,route){ const cfg = readConfigEditor();
  await postJSON('/api/portal/service/node/config',{node,route,config:cfg}); }
async function nodeAction(node,action,extra){ await postJSON('/api/portal/service/node/action',{node,action,...extra}); }
// Log
async function loadLog(){ const d = await getJSON('/api/portal/service/log'); renderLog(d.log); }
```
Editor de config: renderizar os campos do JSON de `buildStationConfig` (escalares editáveis + arrays de sensores/intertravamentos/pinos; `version`/`role`/`configEpoch`/`boundGateway` como read-only). O seletor de rota (via gateway / direta) acompanha o botão Gravar.

- [ ] **Step 3: Commit**
```bash
git add data/irrigacao/portal/index.html data/irrigacao/portal/app.js
git commit -m "feat(irrigation): 8c frontend — abas Clientes/Rede/Log do portal SERVICO"
```

---

## Task 15: Closeout — suíte completa + roadmap + verificação

**Files:**
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` — marcar 8c concluída.

- [ ] **Step 1: Rodar a suíte nativa completa via Docker → GREEN**

Run: `./bin/run-tests.sh` (Docker, memória `windows-native-test-docker`)
Expected: `RESULT: GREEN 60/60 suites passed [canonical: 60/60]` (exit 0). Anotar a contagem de casos (baseline 8b 897 + os casos novos de `test_service_portal`).

- [ ] **Step 2: Marcar 8c no roadmap** — atualizar a seção da Fase 8 em `2026-07-11-irrigacao-roadmap.md` (8c concluída; itens entregues; OUT restantes 8d/follow-ons).

- [ ] **Step 3: Commit + push ao fork**
```bash
git add docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "docs(irrigation): marca Fase 8c concluida no roadmap"
git push fork sistema-irrigacao
```

- [ ] **Step 4: Verificação final** — usar superpowers:finishing-a-development-branch:
  - Suíte nativa GREEN (Step 1) — evidência colada.
  - **Banca 2+ nós EXIGIDA antes de campo:** GET/SET_CONFIG por rádio, re-tune, varredura, ação por nó e import/export por HTTP não são native-testáveis. Listar o que a banca precisa exercitar (select→reboot→canal novo; ler config de estação→editar→gravar via-gateway e direta; scan; import upload; export download; aba Log).
  - Anotar follow-ups Minor em `.superpowers/sdd/progress.md` (ex.: `gwTimeAdopted` ainda stub 0; cifra PSK; SD import.json; import-no-gateway).

---

## Self-Review (preenchido)

**1. Spec coverage:**
- §11.8 aba Clientes → Tasks 1–2 (list/select), 10 (glue), 12 (endpoints), 14 (UI). ✓
- §11.8 aba Rede (estado dos nós) → Tasks 3 (scan build), 10–11 (glue), 12 (endpoints), 14 (UI). ✓
- §11.8 aba Rede (ler/escrever config, editor cheio) → Tasks 4–6 (build/parse config + req), 11 (read/write glue), 12 (endpoints), 14 (UI). ✓
- §11.8 aba Rede (pulso/zona/aprovar-par/resync) → Task 7 (parse), 11 (glue), 12 (endpoint), 14 (UI). ✓
- §11.8 aba Log + writer servico.jsonl → Tasks 8 (build pura), 9 (writer/reader glue), 12 (endpoint), 14 (UI). ✓
- §11.7 vias portal export/import + staging → Task 13. ✓
- Editor: campos geridos read-only → Tasks 4/5 (build informativo + parse preserva). ✓
- Suite nova + contador → Task 1; exclusão nativa da cola → Task 12. ✓

**2. Placeholder scan:** os pontos marcados "casar API real" (getQueryParameter/readBytes/FragmentReassembler/encoders de comando remoto) são **glue de integração validado em banca**, não placeholders de lógica pura — cada um nomeia a função/arquivo de referência existente. `gwTimeAdopted()` entra como stub honesto (retorna 0) com follow-up anotado. Nenhum `TODO/TBD` em código de lógica pura.

**3. Type consistency:** `SvcRoute`/`SvcAction`/`NodeConfigReq`/`NodeAction`/`IServiceLogReader` usados de forma idêntica entre header (Tasks 6/7/8), glue (11) e endpoints (12/13). `buildStationConfig`/`parseStationConfig` casam campo-a-campo (round-trip Task 5 prova). `LightProfile`/`ScanResults`/`ScanEntry` reusados do 8b sem alteração.
