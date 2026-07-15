# Fase 5a — Painel web do gateway — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Servir, no próprio ESP32 do gateway, um painel web (estações, zonas, programas) alimentado por endpoints JSON, reusando a infra web do Meshtastic e mantendo toda a lógica de serialização/parsing em uma camada pura testada nativamente.

**Architecture:** Três camadas. (1) `IrrigationWebApi` — puro C++, sem Arduino/WiFi, faz serialize/parse de JSON à mão e valida; testado no host. (2) `IrrigationWebEndpoints` — cola HTTP só-ESP32 que registra rotas `/api/irrigation/*` no `HTTPServer` do Meshtastic e liga parse→aplicar→responder; validada por build de CI. (3) Frontend estático em LittleFS (`data/`), HTML/CSS/JS puro reproduzindo o mockup, consumindo os endpoints via `fetch()`.

**Tech Stack:** C++17, PlatformIO Unity (suíte nativa `coverage`), `esp32_https_server` (já em árvore via `src/mesh/http/`), LittleFS (`FSCom`), HTML/CSS/vanilla-JS.

## Global Constraints

- Teto absoluto de abertura **120 min** compilado (`IrrigationProto::MAX_OPEN_SECONDS = 120*60`); o painel nunca o contorna.
- Fail-safe local sempre; versão de protocolo em toda mensagem; mismatch = rejeição segura.
- Payload de rádio ≤ ~200 bytes (`IrrigationProto::MAX_PAYLOAD = 200`).
- Convergência por epoch: mudança em config de estação só é "aplicada" após ACK com epoch confirmado (§7.1).
- Tudo do gateway ativo apenas quando `settings.role == IrrigationRole::GATEWAY` (valor `1`).
- Toda a camada `IrrigationWebApi` é **pura**: pode incluir apenas `GatewayTables.h`, `ProgramScheduler.h`, `StationMonitor.h`, `MirrorMode.h`, `IrrigationGateway.h`, `IrrigationSettings.h`, `IrrigationProtocol.h` e headers de `<cstdint>`/`<cstddef>`/`<cstring>`. **Nunca** `Arduino.h`, WiFi, HTTP, `FSCom`.
- A cola HTTP (`IrrigationWebEndpoints.cpp`) e qualquer ponto que a chame ficam sob `#if !MESHTASTIC_EXCLUDE_WEBSERVER` para o build nativo excluí-los.
- Testes: `./bin/run-tests.sh` deve terminar **GREEN**. Ao criar a suíte nova, atualizar `test/native-suite-count` (43 → 44) no mesmo commit, senão o script reporta AMBER.
- Formatar com `trunk fmt` antes de cada commit.
- Idioma do código/comentários: seguir o existente no módulo (comentários em português, identificadores em inglês/português como nos arquivos vizinhos).

---

## File Structure

Criar:

- `src/modules/irrigation/StationTelemetryCache.h` / `.cpp` — cache puro do último heartbeat por estação.
- `src/modules/irrigation/IrrigationWebApi.h` / `.cpp` — serializadores + parsers JSON (puro).
- `src/modules/irrigation/IrrigationWebEndpoints.h` / `.cpp` — cola HTTP (só ESP32).
- `test/test_irrigation_webapi/test_main.cpp` — suíte nativa nova.
- `data/irrigacao/index.html`, `data/irrigacao/app.js`, `data/irrigacao/style.css` — frontend estático.

Modificar:

- `src/modules/irrigation/MirrorMode.h` / `.cpp` — helper `mirrorOwnsZoneOutput` (Task 1).
- `src/modules/irrigation/GatewayTables.h` / `.cpp` — `StationRegistry::nodeAt` + `static_assert` (Task 2).
- `src/modules/irrigation/IrrigationModule.cpp` — usar helper em `gwTick` (Task 1), rewire silêncio (Task 2), alimentar telemetry cache (Task 8), métodos de serviço públicos (Task 9).
- `src/modules/irrigation/IrrigationModule.h` — assinatura dos métodos de serviço + membro do cache (Tasks 8, 9).
- `src/modules/irrigation/IrrigationGateway.h` — adicionar `StationTelemetryCache telemetry;` (Task 8).
- `test/test_irrigation_mirror/test_main.cpp` — teste do helper (Task 1).
- `test/test_irrigation_gwtables/test_main.cpp` — teste de `nodeAt` + telemetry cache (Tasks 2, 8).
- `src/mesh/http/ContentHandler.cpp` — 1 chamada guardada a `registerIrrigationHandlers` (Task 10).
- `test/native-suite-count` — 43 → 44 (Task 3).

---

## Task 1: Suprimir CLOSE do scheduler quando o espelho é dono da zona (carryover F4 #1)

O `gwTick` suprime o `OPEN` do scheduler quando o mirror está ativo naquela entrada (`IrrigationModule.cpp:997`), mas **não** suprime o `CLOSE` (`:1010`) — o fim de etapa do scheduler fecha a válvula que o espelho mantém aberta. Extrair a condição para um helper puro testável e aplicá-la nos dois ramos.

**Files:**
- Modify: `src/modules/irrigation/MirrorMode.h`
- Modify: `src/modules/irrigation/MirrorMode.cpp`
- Modify: `src/modules/irrigation/IrrigationModule.cpp:995-1011`
- Test: `test/test_irrigation_mirror/test_main.cpp`

**Interfaces:**
- Produces: `bool mirrorOwnsZoneOutput(const MirrorMode &m, int8_t fonteInput);` — free function no header de MirrorMode. `true` sse `m.enabled() && fonteInput >= 0 && m.inputActive((uint8_t)fonteInput)`.

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_mirror/test_main.cpp`, adicionar (e registrar no `RUN_TEST` do `main`):

```cpp
static void test_mirrorOwnsZoneOutput_predicate()
{
    MirrorMode m;
    // desabilitado: nunca é dono
    TEST_ASSERT_FALSE(mirrorOwnsZoneOutput(m, 0));
    m.setEnabled(true);
    // sem fonte (-1): não é dono
    TEST_ASSERT_FALSE(mirrorOwnsZoneOutput(m, -1));
    // entrada 0 ativa (bitmap 0x01) após debounce
    uint32_t t = 1000;
    m.update(0x01, t);
    t += MirrorMode::DEBOUNCE_MS + 1;
    m.update(0x01, t); // estabiliza -> inputActive(0) true
    TEST_ASSERT_TRUE(m.inputActive(0));
    TEST_ASSERT_TRUE(mirrorOwnsZoneOutput(m, 0));
    TEST_ASSERT_FALSE(mirrorOwnsZoneOutput(m, 1)); // outra entrada, inativa
}
```

- [ ] **Step 2: Rodar e ver falhar (função não existe)**

Run: `./bin/run-tests.sh -f test_irrigation_mirror`
Expected: RED — `mirrorOwnsZoneOutput` não declarado.

- [ ] **Step 3: Declarar o helper no header**

Em `src/modules/irrigation/MirrorMode.h`, após a definição da classe `MirrorMode` (antes do último `#endif`/EOF, fora da classe):

```cpp
// Helper puro: o espelho é "dono" da saída desta zona? (bypass do scheduler nos dois
// sentidos — OPEN e CLOSE). fonteInput = Zone::fonteInput (-1 = zona não espelhada).
bool mirrorOwnsZoneOutput(const MirrorMode &m, int8_t fonteInput);
```

- [ ] **Step 4: Implementar no .cpp**

Em `src/modules/irrigation/MirrorMode.cpp`, no fim do arquivo:

```cpp
bool mirrorOwnsZoneOutput(const MirrorMode &m, int8_t fonteInput)
{
    return m.enabled() && fonteInput >= 0 && m.inputActive((uint8_t)fonteInput);
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_mirror`
Expected: FILTERED, sem falhas (o helper passa).

- [ ] **Step 6: Aplicar o helper nos dois ramos do gwTick**

Em `src/modules/irrigation/IrrigationModule.cpp`, substituir o bloco `OPEN`/`CLOSE` (linhas ~995-1011):

```cpp
            if (a.type == SchedAction::Type::OPEN) {
                // Bypass: se o espelho é dono desta zona, ele manda — suprime o OPEN.
                if (mirrorOwnsZoneOutput(gateway.mirror, z->fonteInput)) {
                    LOG_DEBUG("Irrigation GW: scheduler OPEN zone=%u suprimido (espelho dono)", a.zoneId);
                    continue;
                }
                uint16_t dur = a.durationS;
                if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60))
                    dur = (uint16_t)(z->maxMin * 60);
                const StationEntry *stEntry = gateway.stations.byNode(z->node);
                uint8_t attempts = (stEntry && stEntry->retries > 0) ? stEntry->retries : 3;
                gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, a.zoneId, attempts);
            } else { // CLOSE
                // Mesma proteção: não feche o que o espelho mantém aberto (carryover F4 #1).
                if (mirrorOwnsZoneOutput(gateway.mirror, z->fonteInput)) {
                    LOG_DEBUG("Irrigation GW: scheduler CLOSE zone=%u suprimido (espelho dono)", a.zoneId);
                    continue;
                }
                gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, a.zoneId, 1);
            }
```

- [ ] **Step 7: Rodar suíte completa**

Run: `./bin/run-tests.sh`
Expected: `RESULT: GREEN 43/43 suites passed`.

- [ ] **Step 8: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/MirrorMode.h src/modules/irrigation/MirrorMode.cpp src/modules/irrigation/IrrigationModule.cpp test/test_irrigation_mirror/test_main.cpp
git add src/modules/irrigation/MirrorMode.h src/modules/irrigation/MirrorMode.cpp src/modules/irrigation/IrrigationModule.cpp test/test_irrigation_mirror/test_main.cpp
git commit -m "fix(irrigation): suppress scheduler CLOSE when mirror owns the zone (F4 carryover)"
```

---

## Task 2: `StationRegistry::nodeAt` + `static_assert` settings↔blob (carryover F4 #2, #3)

A varredura de silêncio (`IrrigationModule.cpp:1093-1114`) itera a allowlist em paralelo para achar nós e consulta o registry — frágil. Dar ao registry seu próprio iterador compactado e reescrever a varredura. Adicionar o `static_assert` que trava `sizeof(IrrigationSettings)` ao tamanho do `blob` de config empurrada.

**Files:**
- Modify: `src/modules/irrigation/GatewayTables.h`
- Modify: `src/modules/irrigation/GatewayTables.cpp`
- Modify: `src/modules/irrigation/IrrigationModule.cpp:1093-1114`
- Test: `test/test_irrigation_gwtables/test_main.cpp`

**Interfaces:**
- Produces: `const StationEntry *StationRegistry::nodeAt(size_t index) const;` — devolve a `index`-ésima entrada **ocupada** (node != 0), em ordem de slot; `nullptr` quando `index >= count()`.

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_gwtables/test_main.cpp` (registrar no `main`):

```cpp
static void test_stations_nodeAtCompacted()
{
    StationRegistry r;
    StationEntry a; a.node = 0xAA; snprintf(a.name, sizeof(a.name), "A");
    StationEntry b; b.node = 0xBB; snprintf(b.name, sizeof(b.name), "B");
    TEST_ASSERT_TRUE(r.upsert(a));
    TEST_ASSERT_TRUE(r.upsert(b));
    TEST_ASSERT_TRUE(r.removeByNode(0xAA)); // abre buraco no slot 0
    TEST_ASSERT_EQUAL_UINT(1, r.count());
    const StationEntry *e0 = r.nodeAt(0);
    TEST_ASSERT_NOT_NULL(e0);
    TEST_ASSERT_EQUAL_HEX32(0xBB, e0->node); // compactado: pula o buraco
    TEST_ASSERT_NULL(r.nodeAt(1));           // fora do count
    TEST_ASSERT_NULL(r.nodeAt(99));
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_irrigation_gwtables`
Expected: RED — `nodeAt` não é membro de `StationRegistry`.

- [ ] **Step 3: Declarar no header**

Em `src/modules/irrigation/GatewayTables.h`, dentro de `class StationRegistry`, após `mutableByNode`:

```cpp
    const StationEntry *nodeAt(size_t index) const; // index-ésima entrada ocupada; nullptr se >= count()
```

- [ ] **Step 4: Implementar no .cpp**

Em `src/modules/irrigation/GatewayTables.cpp`, adicionar (junto aos outros métodos de `StationRegistry`) e o `static_assert` no topo do arquivo (após os includes):

```cpp
#include "modules/irrigation/IrrigationSettings.h"
static_assert(sizeof(IrrigationSettings) <= sizeof(((StationEntry *)nullptr)->blob),
              "IrrigationSettings excede StationEntry::blob — a adoção de config falharia em silêncio; "
              "aumente blob[] e bumpe a versão");
```

```cpp
const StationEntry *StationRegistry::nodeAt(size_t index) const
{
    size_t seen = 0;
    for (size_t i = 0; i < MAX; i++) {
        if (stations[i].node == 0)
            continue;
        if (seen == index)
            return &stations[i];
        seen++;
    }
    return nullptr;
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_gwtables`
Expected: FILTERED, sem falhas.

- [ ] **Step 6: Reescrever a varredura de silêncio usando o iterador**

Em `src/modules/irrigation/IrrigationModule.cpp`, substituir o loop de silêncio (linhas ~1093-1114) por:

```cpp
    // --- Silêncio por estação (itera o registry diretamente, não em paralelo à allowlist) ---
    for (size_t i = 0; i < gateway.stations.count(); i++) {
        const StationEntry *entry = gateway.stations.nodeAt(i);
        if (!entry)
            break;
        uint32_t silMs = (uint32_t)entry->silencioAlertaMin * 60000UL;
        Alert a;
        if (gateway.monitor.checkSilence(entry->node, silMs, millis(), a)) {
            gateway.alerts.push(a);
            LOG_WARN("Irrigation GW: SILENT node=0x%08x", entry->node);
        }
    }
```

- [ ] **Step 7: Rodar suíte completa**

Run: `./bin/run-tests.sh`
Expected: `RESULT: GREEN 43/43 suites passed`.

- [ ] **Step 8: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/GatewayTables.h src/modules/irrigation/GatewayTables.cpp src/modules/irrigation/IrrigationModule.cpp test/test_irrigation_gwtables/test_main.cpp
git add src/modules/irrigation/GatewayTables.h src/modules/irrigation/GatewayTables.cpp src/modules/irrigation/IrrigationModule.cpp test/test_irrigation_gwtables/test_main.cpp
git commit -m "refactor(irrigation): StationRegistry::nodeAt iterator + settings/blob static_assert (F4 carryover)"
```

---

## Task 3: Suíte nova + JSON writer + `computeSync` + `buildOverview`

Primeiro pedaço da camada pura. Cria a suíte, o writer mínimo, o helper de sincronização e o serializador de visão geral.

**Files:**
- Create: `src/modules/irrigation/IrrigationWebApi.h`
- Create: `src/modules/irrigation/IrrigationWebApi.cpp`
- Create: `test/test_irrigation_webapi/test_main.cpp`
- Modify: `test/native-suite-count`

**Interfaces:**
- Produces:
  - `namespace IrrigationWeb {`
  - `enum class SyncState : uint8_t { SINCRONIZADA = 0, PENDENTE = 1, INALCANCAVEL = 2 };`
  - `SyncState computeSync(uint32_t desiredEpoch, uint32_t reportedEpoch, bool silent);`
  - `struct OverviewCtx { bool hasRtc; uint16_t stationCount; uint8_t running; uint16_t alertCount; bool pairingPending; uint32_t pairingNodeId; uint16_t pairingSecondsLeft; uint8_t runningZoneId; uint16_t runningRemainMin; };`
  - `size_t buildOverview(const OverviewCtx &ctx, char *buf, size_t cap);` — escreve JSON, devolve bytes escritos (sem o `\0`), `0` se `cap` insuficiente.
  - Internamente, uma classe `JsonWriter` (ver Step 3) — pode ficar no `.cpp` como detalhe, mas exponha-a no header se Task 5 for reusá-la (é; então declare no header).

- [ ] **Step 1: Registrar a suíte + escrever o primeiro teste (falha)**

Atualizar `test/native-suite-count` para `44`.

Criar `test/test_irrigation_webapi/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationWebApi.h"
#include <string.h>
#include <unity.h>

using namespace IrrigationWeb;

void setUp(void) {}
void tearDown(void) {}

static bool contains(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }

static void test_computeSync_states()
{
    TEST_ASSERT_EQUAL(SyncState::INALCANCAVEL, computeSync(5, 5, true));
    TEST_ASSERT_EQUAL(SyncState::SINCRONIZADA, computeSync(5, 5, false));
    TEST_ASSERT_EQUAL(SyncState::PENDENTE, computeSync(6, 5, false));
    TEST_ASSERT_EQUAL(SyncState::PENDENTE, computeSync(1, 0, false));
}

static void test_buildOverview_json()
{
    OverviewCtx c = {};
    c.hasRtc = false;
    c.stationCount = 3;
    c.running = 0;
    c.alertCount = 2;
    c.pairingPending = true;
    c.pairingNodeId = 0xABCD;
    c.pairingSecondsLeft = 90;
    char buf[512];
    size_t n = buildOverview(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"hasRtc\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"stationCount\":3"));
    TEST_ASSERT_TRUE(contains(buf, "\"alertCount\":2"));
    TEST_ASSERT_TRUE(contains(buf, "\"pairingPending\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"pairingNodeId\":43981")); // 0xABCD
}

static void test_buildOverview_truncationReturnsZero()
{
    OverviewCtx c = {};
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(0, buildOverview(c, buf, sizeof(buf)));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_computeSync_states);
    RUN_TEST(test_buildOverview_json);
    RUN_TEST(test_buildOverview_truncationReturnsZero);
    return UNITY_END();
}
```

- [ ] **Step 2: Rodar e ver falhar (header inexistente)**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED — `IrrigationWebApi.h` não encontrado.

- [ ] **Step 3: Criar o header com o writer e as assinaturas**

Criar `src/modules/irrigation/IrrigationWebApi.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

namespace IrrigationWeb
{

// Writer JSON mínimo: escreve em um buffer fixo do chamador. overflow() vira true e
// todas as escritas subsequentes viram no-op; done() devolve bytes escritos ou 0 se
// houve overflow. Sem alocação, sem dependência externa.
class JsonWriter
{
  public:
    JsonWriter(char *buf, size_t cap) : _b(buf), _cap(cap) {}
    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(const char *k);          // escreve "k": e arma vírgula pós-valor
    void str(const char *v);          // valor string (com escape básico)
    void num(int64_t v);              // valor inteiro
    void boolean(bool v);
    void raw(const char *v);          // valor já-JSON (ex.: objeto aninhado montado à parte)
    void keyStr(const char *k, const char *v) { key(k); str(v); }
    void keyNum(const char *k, int64_t v) { key(k); num(v); }
    void keyBool(const char *k, bool v) { key(k); boolean(v); }
    bool overflow() const { return _ovf; }
    size_t done();                    // fecha nada; devolve _len (0 se overflow)

  private:
    void putc_(char c);
    void puts_(const char *s);
    void sep_();                      // vírgula entre itens conforme estado
    char *_b;
    size_t _cap;
    size_t _len = 0;
    bool _ovf = false;
    bool _needComma = false;
};

enum class SyncState : uint8_t { SINCRONIZADA = 0, PENDENTE = 1, INALCANCAVEL = 2 };
SyncState computeSync(uint32_t desiredEpoch, uint32_t reportedEpoch, bool silent);
const char *syncLabel(SyncState s); // "sincronizada" | "pendente" | "inalcancavel"

struct OverviewCtx {
    bool hasRtc = false;
    uint16_t stationCount = 0;
    uint8_t running = 0; // 0 = ocioso, 1 = executando
    uint16_t alertCount = 0;
    bool pairingPending = false;
    uint32_t pairingNodeId = 0;
    uint16_t pairingSecondsLeft = 0;
    uint8_t runningZoneId = 0;
    uint16_t runningRemainMin = 0;
};
size_t buildOverview(const OverviewCtx &ctx, char *buf, size_t cap);

} // namespace IrrigationWeb
```

- [ ] **Step 4: Implementar writer + computeSync + buildOverview**

Criar `src/modules/irrigation/IrrigationWebApi.cpp`:

```cpp
#include "modules/irrigation/IrrigationWebApi.h"
#include <cstdio>
#include <cstring>

namespace IrrigationWeb
{

void JsonWriter::putc_(char c)
{
    if (_ovf) return;
    if (_len + 1 >= _cap) { _ovf = true; return; } // +1 mantém espaço pro '\0'
    _b[_len++] = c;
    _b[_len] = '\0';
}
void JsonWriter::puts_(const char *s) { while (*s) putc_(*s++); }
void JsonWriter::sep_() { if (_needComma) putc_(','); _needComma = false; }

void JsonWriter::beginObject() { sep_(); putc_('{'); }
void JsonWriter::endObject() { putc_('}'); _needComma = true; }
void JsonWriter::beginArray() { sep_(); putc_('['); }
void JsonWriter::endArray() { putc_(']'); _needComma = true; }
void JsonWriter::key(const char *k) { sep_(); putc_('"'); puts_(k); puts_("\":"); _needComma = false; }
void JsonWriter::str(const char *v)
{
    putc_('"');
    for (const char *p = v; *p; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') { putc_('\\'); putc_(c); }
        else if (c == '\n') { putc_('\\'); putc_('n'); }
        else if ((unsigned char)c < 0x20) { /* controle: descarta */ }
        else putc_(c);
    }
    putc_('"');
    _needComma = true;
}
void JsonWriter::num(int64_t v) { char t[24]; snprintf(t, sizeof(t), "%lld", (long long)v); puts_(t); _needComma = true; }
void JsonWriter::boolean(bool v) { puts_(v ? "true" : "false"); _needComma = true; }
void JsonWriter::raw(const char *v) { sep_(); puts_(v); _needComma = true; }
size_t JsonWriter::done() { return _ovf ? 0 : _len; }

SyncState computeSync(uint32_t desiredEpoch, uint32_t reportedEpoch, bool silent)
{
    if (silent) return SyncState::INALCANCAVEL;
    return (desiredEpoch != reportedEpoch) ? SyncState::PENDENTE : SyncState::SINCRONIZADA;
}
const char *syncLabel(SyncState s)
{
    switch (s) {
        case SyncState::SINCRONIZADA: return "sincronizada";
        case SyncState::PENDENTE: return "pendente";
        default: return "inalcancavel";
    }
}

size_t buildOverview(const OverviewCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyBool("hasRtc", ctx.hasRtc);
    w.keyNum("stationCount", ctx.stationCount);
    w.keyNum("running", ctx.running);
    w.keyNum("runningZoneId", ctx.runningZoneId);
    w.keyNum("runningRemainMin", ctx.runningRemainMin);
    w.keyNum("alertCount", ctx.alertCount);
    w.keyBool("pairingPending", ctx.pairingPending);
    w.keyNum("pairingNodeId", (int64_t)ctx.pairingNodeId);
    w.keyNum("pairingSecondsLeft", ctx.pairingSecondsLeft);
    w.endObject();
    return w.done();
}

} // namespace IrrigationWeb
```

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, 3 testes PASS.

- [ ] **Step 6: Rodar suíte completa (checa o count 44)**

Run: `./bin/run-tests.sh`
Expected: `RESULT: GREEN 44/44 suites passed`.

- [ ] **Step 7: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): webapi pure layer — json writer, computeSync, buildOverview"
```

---

## Task 4: `buildStations`

Serializador de estações a partir de uma view montada pelo módulo (o assembly de estado fica na cola; o serializador é puro e testado com arrays sintéticos).

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `SyncState`, `JsonWriter` (Task 3).
- Produces:
  - `struct StationView { uint32_t node; const char *name; SyncState sync; uint32_t secsSinceHeard; uint16_t vbatCentiV; uint16_t vpanelCentiV; int8_t snrQuarterDb; uint16_t rebootCount; uint8_t flags; int32_t lat; int32_t lon; };`
  - `size_t buildStations(const StationView *views, size_t n, char *buf, size_t cap);`

- [ ] **Step 1: Escrever o teste que falha**

Adicionar em `test/test_irrigation_webapi/test_main.cpp` (e no `main`):

```cpp
static void test_buildStations_json()
{
    StationView v[2] = {};
    v[0].node = 0x1111; v[0].name = "Pasto Norte"; v[0].sync = SyncState::SINCRONIZADA;
    v[0].secsSinceHeard = 120; v[0].vbatCentiV = 1250; v[0].vpanelCentiV = 1800;
    v[0].snrQuarterDb = 40; v[0].rebootCount = 2; v[0].flags = 0; v[0].lat = -2212340; v[0].lon = -4765430;
    v[1].node = 0x2222; v[1].name = "Horta"; v[1].sync = SyncState::INALCANCAVEL;
    v[1].secsSinceHeard = 4000;
    char buf[1024];
    size_t n = buildStations(v, 2, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"node\":4369"));            // 0x1111
    TEST_ASSERT_TRUE(contains(buf, "\"name\":\"Pasto Norte\""));
    TEST_ASSERT_TRUE(contains(buf, "\"sync\":\"sincronizada\""));
    TEST_ASSERT_TRUE(contains(buf, "\"sync\":\"inalcancavel\""));
    TEST_ASSERT_TRUE(contains(buf, "\"vbatCentiV\":1250"));
    TEST_ASSERT_TRUE(contains(buf, "\"lat\":-2212340"));
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED — `buildStations`/`StationView` inexistentes.

- [ ] **Step 3: Declarar no header**

Em `IrrigationWebApi.h`, dentro do namespace, após `buildOverview`:

```cpp
struct StationView {
    uint32_t node = 0;
    const char *name = "";
    SyncState sync = SyncState::SINCRONIZADA;
    uint32_t secsSinceHeard = 0;
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0;
    int8_t snrQuarterDb = 0;
    uint16_t rebootCount = 0;
    uint8_t flags = 0; // HbFlags: tamper/safe/hibernation
    int32_t lat = 0, lon = 0;
};
size_t buildStations(const StationView *views, size_t n, char *buf, size_t cap);
```

- [ ] **Step 4: Implementar**

Em `IrrigationWebApi.cpp`, após `buildOverview`:

```cpp
size_t buildStations(const StationView *views, size_t n, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const StationView &v = views[i];
        w.beginObject();
        w.keyNum("node", (int64_t)v.node);
        w.keyStr("name", v.name);
        w.keyStr("sync", syncLabel(v.sync));
        w.keyNum("secsSinceHeard", v.secsSinceHeard);
        w.keyNum("vbatCentiV", v.vbatCentiV);
        w.keyNum("vpanelCentiV", v.vpanelCentiV);
        w.keyNum("snrQuarterDb", v.snrQuarterDb);
        w.keyNum("rebootCount", v.rebootCount);
        w.keyNum("flags", v.flags);
        w.keyNum("lat", v.lat);
        w.keyNum("lon", v.lon);
        w.endObject();
    }
    w.endArray();
    return w.done();
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, todos PASS.

- [ ] **Step 6: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): webapi buildStations serializer"
```

---

## Task 5: `buildZones` + `buildPrograms`

Serializam direto das tabelas puras (`ZoneTable`, `ProgramScheduler`).

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `JsonWriter`; `ZoneTable`/`Zone` (`GatewayTables.h`); `ProgramScheduler`/`Program` (`ProgramScheduler.h`).
- Produces:
  - `size_t buildZones(const ZoneTable &zones, char *buf, size_t cap);`
  - `size_t buildPrograms(const ProgramScheduler &sched, char *buf, size_t cap);`
- Nota: para iterar programas, `ProgramScheduler` não expõe iterador público. Adicionar `const Program *ProgramScheduler::programAt(size_t index) const;` (index-ésimo com `id != 0`) — declarar em `ProgramScheduler.h` e implementar em `ProgramScheduler.cpp`, espelhando `ZoneTable`. Para zonas, usar `ZoneTable::MAX` + `byId`? Não — `ZoneTable` também precisa iterar; adicionar `const Zone *ZoneTable::zoneAt(size_t index) const;` da mesma forma.

- [ ] **Step 1: Escrever os testes que falham**

Adicionar em `test/test_irrigation_webapi/test_main.cpp` (e no `main`):

```cpp
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/ProgramScheduler.h"

static void test_buildZones_json()
{
    ZoneTable t;
    Zone z; z.id = 1; snprintf(z.name, sizeof(z.name), "Horta");
    z.node = 0x1111; z.tipo = 0; z.index = 2; z.maxMin = 45; z.padraoMin = 20; z.fonteInput = -1;
    TEST_ASSERT_TRUE(t.upsert(z));
    char buf[1024];
    size_t n = buildZones(t, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"name\":\"Horta\""));
    TEST_ASSERT_TRUE(contains(buf, "\"node\":4369"));
    TEST_ASSERT_TRUE(contains(buf, "\"tipo\":0"));
    TEST_ASSERT_TRUE(contains(buf, "\"index\":2"));
    TEST_ASSERT_TRUE(contains(buf, "\"maxMin\":45"));
    TEST_ASSERT_TRUE(contains(buf, "\"fonteInput\":-1"));
}

static void test_buildPrograms_json()
{
    ProgramScheduler s;
    Program p; p.id = 1; p.enabled = true; p.daysMask = 0x7F; p.startMinute = 360; p.stepCount = 1;
    p.steps[0].zoneId = 1; p.steps[0].durationMin = 15;
    TEST_ASSERT_TRUE(s.upsert(p));
    char buf[1024];
    size_t n = buildPrograms(s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"enabled\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"daysMask\":127"));
    TEST_ASSERT_TRUE(contains(buf, "\"startMinute\":360"));
    TEST_ASSERT_TRUE(contains(buf, "\"zoneId\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"durationMin\":15"));
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED — `buildZones`/`buildPrograms`/`zoneAt`/`programAt` inexistentes.

- [ ] **Step 3: Adicionar iteradores nas tabelas**

Em `src/modules/irrigation/GatewayTables.h`, em `class ZoneTable`, após `byFonte`:

```cpp
    const Zone *zoneAt(size_t index) const; // index-ésima zona ocupada; nullptr se >= count()
```

Em `src/modules/irrigation/GatewayTables.cpp`:

```cpp
const Zone *ZoneTable::zoneAt(size_t index) const
{
    size_t seen = 0;
    for (size_t i = 0; i < MAX; i++) {
        if (zones[i].id == 0)
            continue;
        if (seen == index)
            return &zones[i];
        seen++;
    }
    return nullptr;
}
```

Em `src/modules/irrigation/ProgramScheduler.h`, em `class ProgramScheduler`, após `count()`:

```cpp
    const Program *programAt(size_t index) const; // index-ésimo programa ocupado; nullptr se >= count()
```

Em `src/modules/irrigation/ProgramScheduler.cpp`:

```cpp
const Program *ProgramScheduler::programAt(size_t index) const
{
    size_t seen = 0;
    for (size_t i = 0; i < MAX_PROGRAMS; i++) {
        if (programs[i].id == 0)
            continue;
        if (seen == index)
            return &programs[i];
        seen++;
    }
    return nullptr;
}
```

- [ ] **Step 4: Declarar os serializadores no header**

Em `IrrigationWebApi.h` (adicionar os includes das tabelas no topo do header — são puros):

```cpp
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/ProgramScheduler.h"
```

Dentro do namespace:

```cpp
size_t buildZones(const ZoneTable &zones, char *buf, size_t cap);
size_t buildPrograms(const ProgramScheduler &sched, char *buf, size_t cap);
```

- [ ] **Step 5: Implementar os serializadores**

Em `IrrigationWebApi.cpp`:

```cpp
size_t buildZones(const ZoneTable &zones, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < zones.count(); i++) {
        const Zone *z = zones.zoneAt(i);
        if (!z) break;
        w.beginObject();
        w.keyNum("id", z->id);
        w.keyStr("name", z->name);
        w.keyNum("node", (int64_t)z->node);
        w.keyNum("tipo", z->tipo);
        w.keyNum("index", z->index);
        w.keyNum("maxMin", z->maxMin);
        w.keyNum("padraoMin", z->padraoMin);
        w.keyNum("fonteInput", z->fonteInput);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

size_t buildPrograms(const ProgramScheduler &sched, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < sched.count(); i++) {
        const Program *p = sched.programAt(i);
        if (!p) break;
        w.beginObject();
        w.keyNum("id", p->id);
        w.keyBool("enabled", p->enabled);
        w.keyNum("daysMask", p->daysMask);
        w.keyNum("startMinute", p->startMinute);
        w.key("steps");
        w.beginArray();
        for (uint8_t s = 0; s < p->stepCount && s < 8; s++) {
            w.beginObject();
            w.keyNum("zoneId", p->steps[s].zoneId);
            w.keyNum("durationMin", p->steps[s].durationMin);
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();
    return w.done();
}
```

- [ ] **Step 6: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, todos PASS.

- [ ] **Step 7: Rodar suíte completa (mexemos em headers compartilhados)**

Run: `./bin/run-tests.sh`
Expected: `RESULT: GREEN 44/44 suites passed`.

- [ ] **Step 8: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/GatewayTables.h src/modules/irrigation/GatewayTables.cpp src/modules/irrigation/ProgramScheduler.h src/modules/irrigation/ProgramScheduler.cpp src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add -A
git commit -m "feat(irrigation): webapi buildZones/buildPrograms + table iterators"
```

---

## Task 6: JSON reader + `parseZoneUpsert` + `parseZoneDelete`

Reader mínimo de JSON de objeto plano (chaves→número/string/bool) e os dois parsers de zona, com validação.

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Produces:
  - `struct ParseError { char msg[48]; };`
  - `struct ParseResult { bool ok; uint8_t errorCount; ParseError errors[4]; };` com helper interno para `addError`.
  - `ParseResult parseZoneUpsert(const char *json, size_t len, Zone &out);`
  - `ParseResult parseZoneDelete(const char *json, size_t len, uint8_t &outId);`
  - Reader: `class JsonReader` com `bool getInt(const char *key, int64_t &out) const;` `bool getStr(const char *key, char *out, size_t cap) const;` `bool getBool(const char *key, bool &out) const;` — busca a chave no nível superior de um objeto plano. Constrói-se de `JsonReader(const char *json, size_t len)`.

Regras de validação de zona (reusam limites das tabelas):
- `id` obrigatório, `1..255` (0 = inválido; `ZoneTable::upsert` já rejeita 0).
- `name` obrigatório, não vazio, ≤ 15 chars (cabe em `Zone::name[16]`).
- `node` obrigatório, `!= 0`.
- `tipo` `0` ou `1`.
- `index` `0..7` (`IrrigationSettings::MAX_VALVES-1`).
- `maxMin` `1..120` (teto compilado; §4.2). `padraoMin` `1..maxMin`.
- `fonteInput` `-1..3` (`MirrorMode::INPUTS-1`).

- [ ] **Step 1: Escrever os testes que falham**

Adicionar (e registrar no `main`):

```cpp
static void test_parseZoneUpsert_ok()
{
    const char *j = "{\"id\":3,\"name\":\"Horta\",\"node\":4369,\"tipo\":0,\"index\":2,\"maxMin\":45,\"padraoMin\":20,\"fonteInput\":-1}";
    Zone z;
    ParseResult r = parseZoneUpsert(j, strlen(j), z);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(3, z.id);
    TEST_ASSERT_EQUAL_STRING("Horta", z.name);
    TEST_ASSERT_EQUAL_HEX32(4369, z.node);
    TEST_ASSERT_EQUAL_UINT16(45, z.maxMin);
    TEST_ASSERT_EQUAL_INT8(-1, z.fonteInput);
}

static void test_parseZoneUpsert_rejectsBadRange()
{
    Zone z;
    const char *j1 = "{\"id\":0,\"name\":\"X\",\"node\":1,\"tipo\":0,\"index\":0,\"maxMin\":10,\"padraoMin\":5}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j1, strlen(j1), z).ok); // id 0
    const char *j2 = "{\"id\":1,\"name\":\"X\",\"node\":1,\"tipo\":0,\"index\":0,\"maxMin\":200,\"padraoMin\":5}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j2, strlen(j2), z).ok); // maxMin > 120
    const char *j3 = "{\"id\":1,\"name\":\"\",\"node\":1,\"tipo\":0,\"index\":0,\"maxMin\":10,\"padraoMin\":5}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j3, strlen(j3), z).ok); // nome vazio
    const char *j4 = "{\"id\":1,\"name\":\"X\",\"node\":0,\"tipo\":0,\"index\":0,\"maxMin\":10,\"padraoMin\":5}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j4, strlen(j4), z).ok); // node 0
    const char *j5 = "{\"id\":1,\"name\":\"X\",\"node\":1,\"tipo\":0,\"index\":0,\"maxMin\":10,\"padraoMin\":50}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j5, strlen(j5), z).ok); // padrao > max
}

static void test_parseZoneDelete_ok()
{
    const char *j = "{\"id\":7}";
    uint8_t id = 0;
    TEST_ASSERT_TRUE(parseZoneDelete(j, strlen(j), id).ok);
    TEST_ASSERT_EQUAL_UINT8(7, id);
    const char *jb = "{\"id\":0}";
    TEST_ASSERT_FALSE(parseZoneDelete(jb, strlen(jb), id).ok);
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED — parsers/reader inexistentes.

- [ ] **Step 3: Declarar reader + tipos de resultado no header**

Em `IrrigationWebApi.h`, dentro do namespace:

```cpp
struct ParseError {
    char msg[48];
};
struct ParseResult {
    bool ok = true;
    uint8_t errorCount = 0;
    ParseError errors[4];
    void fail(const char *m);
};

// Reader de objeto JSON plano: só o nível superior, valores número/string/bool.
// Sem aninhamento (arrays/objetos são ignorados pelas getters escalares). Suficiente
// para os corpos que o painel envia. Para programas (com array), ver Task 7.
class JsonReader
{
  public:
    JsonReader(const char *json, size_t len) : _j(json), _len(len) {}
    bool getInt(const char *key, int64_t &out) const;
    bool getStr(const char *key, char *out, size_t cap) const;
    bool getBool(const char *key, bool &out) const;

  private:
    const char *findValue(const char *key) const; // aponta pro 1º char do valor, ou nullptr
    const char *_j;
    size_t _len;
};

ParseResult parseZoneUpsert(const char *json, size_t len, Zone &out);
ParseResult parseZoneDelete(const char *json, size_t len, uint8_t &outId);
```

- [ ] **Step 4: Implementar reader + parsers**

Em `IrrigationWebApi.cpp` (inclui `<cstdlib>` para `strtoll`):

```cpp
void ParseResult::fail(const char *m)
{
    ok = false;
    if (errorCount < 4) {
        snprintf(errors[errorCount].msg, sizeof(errors[errorCount].msg), "%s", m);
        errorCount++;
    }
}

// Busca "key" no nível superior. Estratégia simples: procura a substring "\"key\""
// seguida de ':' e devolve o 1º char não-espaço do valor. Suficiente para objetos
// planos gerados pelo próprio painel (não é um parser JSON completo — nomes de chave
// não contêm os caracteres especiais que iludiriam a busca).
const char *JsonReader::findValue(const char *key) const
{
    char pat[40];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0) return nullptr;
    const char *p = _j;
    const char *end = _j + _len;
    while ((p = strstr(p, pat)) != nullptr && p < end) {
        const char *q = p + pn;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q < end && *q == ':') {
            q++;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            return q;
        }
        p = q;
    }
    return nullptr;
}

bool JsonReader::getInt(const char *key, int64_t &out) const
{
    const char *v = findValue(key);
    if (!v) return false;
    char *end = nullptr;
    long long n = strtoll(v, &end, 10);
    if (end == v) return false;
    out = (int64_t)n;
    return true;
}

bool JsonReader::getBool(const char *key, bool &out) const
{
    const char *v = findValue(key);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0) { out = true; return true; }
    if (strncmp(v, "false", 5) == 0) { out = false; return true; }
    return false;
}

bool JsonReader::getStr(const char *key, char *out, size_t cap) const
{
    const char *v = findValue(key);
    if (!v || *v != '"' || cap == 0) return false;
    v++; // pula a aspa de abertura
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < cap) {
        if (*v == '\\' && v[1]) v++; // escape simples: copia o próximo literal
        out[i++] = *v++;
    }
    out[i] = '\0';
    return (*v == '"'); // só ok se fechou a string
}

ParseResult parseZoneUpsert(const char *json, size_t len, Zone &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0, node = 0, tipo = 0, index = 0, maxMin = 0, padraoMin = 0, fonte = -1;
    char name[16] = {0};
    if (!rd.getInt("id", id) || id < 1 || id > 255) r.fail("id invalido (1..255)");
    if (!rd.getStr("name", name, sizeof(name)) || name[0] == '\0') r.fail("nome vazio/ausente");
    if (!rd.getInt("node", node) || node == 0) r.fail("node ausente/zero");
    if (!rd.getInt("tipo", tipo) || (tipo != 0 && tipo != 1)) r.fail("tipo deve ser 0 ou 1");
    if (!rd.getInt("index", index) || index < 0 || index > 7) r.fail("index fora de 0..7");
    if (!rd.getInt("maxMin", maxMin) || maxMin < 1 || maxMin > 120) r.fail("maxMin fora de 1..120");
    if (!rd.getInt("padraoMin", padraoMin) || padraoMin < 1 || padraoMin > maxMin) r.fail("padraoMin fora de 1..maxMin");
    rd.getInt("fonteInput", fonte); // opcional; default -1
    if (fonte < -1 || fonte > 3) r.fail("fonteInput fora de -1..3");
    if (!r.ok) return r;
    out = Zone{};
    out.id = (uint8_t)id;
    snprintf(out.name, sizeof(out.name), "%s", name);
    out.node = (uint32_t)node;
    out.tipo = (uint8_t)tipo;
    out.index = (uint8_t)index;
    out.maxMin = (uint16_t)maxMin;
    out.padraoMin = (uint16_t)padraoMin;
    out.fonteInput = (int8_t)fonte;
    return r;
}

ParseResult parseZoneDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > 255) { r.fail("id invalido (1..255)"); return r; }
    outId = (uint8_t)id;
    return r;
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, todos PASS.

- [ ] **Step 6: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): webapi json reader + parseZoneUpsert/Delete with validation"
```

---

## Task 7: `parseProgramUpsert` + `parseProgramToggle` + `parseProgramDelete` + `parseCommand`

Parser de programa (com o array `steps`) e o parser de comando (ação avulsa do painel).

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Produces:
  - `ParseResult parseProgramUpsert(const char *json, size_t len, Program &out);`
  - `ParseResult parseProgramToggle(const char *json, size_t len, uint8_t &outId, bool &outEnabled);`
  - `ParseResult parseProgramDelete(const char *json, size_t len, uint8_t &outId);`
  - `enum class CmdKind : uint8_t { NONE, PULSE_TEST, OPEN, CLOSE, ACK_ALERT, APPROVE_PAIRING };`
  - `struct WebCommand { CmdKind kind; uint8_t zoneId; uint16_t durationS; uint32_t node; };`
  - `ParseResult parseCommand(const char *json, size_t len, WebCommand &out);`

Regras:
- Programa: `id 1..255`; `daysMask 0..127`; `startMinute 0..1439`; `steps` de 1..8, cada `zoneId 1..255`, `durationMin 1..120` (teto §4.2). `enabled` opcional (default true).
- Command `kind`: string `"pulse"|"open"|"close"|"ack"|"approve_pairing"`. `pulse` força `durationS=10` (§7.1 "abre 10 s"). `open` exige `zoneId` e `durationS 1..7200`. `close`/`ack` exigem `zoneId`/`node`. `approve_pairing` não exige campos.

- [ ] **Step 1: Escrever os testes que falham**

Adicionar (registrar no `main`):

```cpp
static void test_parseProgramUpsert_ok()
{
    const char *j = "{\"id\":2,\"enabled\":true,\"daysMask\":127,\"startMinute\":360,"
                    "\"steps\":[{\"zoneId\":1,\"durationMin\":15},{\"zoneId\":2,\"durationMin\":20}]}";
    Program p;
    ParseResult r = parseProgramUpsert(j, strlen(j), p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(2, p.id);
    TEST_ASSERT_EQUAL_UINT8(2, p.stepCount);
    TEST_ASSERT_EQUAL_UINT8(1, p.steps[0].zoneId);
    TEST_ASSERT_EQUAL_UINT16(20, p.steps[1].durationMin);
}

static void test_parseProgramUpsert_rejectsTooManySteps()
{
    // 9 etapas > 8
    const char *j = "{\"id\":1,\"daysMask\":1,\"startMinute\":0,\"steps\":["
        "{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},"
        "{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},"
        "{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1}]}";
    Program p;
    TEST_ASSERT_FALSE(parseProgramUpsert(j, strlen(j), p).ok);
}

static void test_parseProgramUpsert_rejectsNoSteps()
{
    const char *j = "{\"id\":1,\"daysMask\":1,\"startMinute\":0,\"steps\":[]}";
    Program p;
    TEST_ASSERT_FALSE(parseProgramUpsert(j, strlen(j), p).ok);
}

static void test_parseProgramToggle_ok()
{
    const char *j = "{\"id\":4,\"enabled\":false}";
    uint8_t id = 0; bool en = true;
    TEST_ASSERT_TRUE(parseProgramToggle(j, strlen(j), id, en).ok);
    TEST_ASSERT_EQUAL_UINT8(4, id);
    TEST_ASSERT_FALSE(en);
}

static void test_parseCommand_kinds()
{
    WebCommand c;
    const char *jp = "{\"kind\":\"pulse\",\"zoneId\":1}";
    TEST_ASSERT_TRUE(parseCommand(jp, strlen(jp), c).ok);
    TEST_ASSERT_EQUAL(CmdKind::PULSE_TEST, c.kind);
    TEST_ASSERT_EQUAL_UINT16(10, c.durationS);

    const char *jo = "{\"kind\":\"open\",\"zoneId\":2,\"durationS\":600}";
    TEST_ASSERT_TRUE(parseCommand(jo, strlen(jo), c).ok);
    TEST_ASSERT_EQUAL(CmdKind::OPEN, c.kind);
    TEST_ASSERT_EQUAL_UINT16(600, c.durationS);

    const char *ja = "{\"kind\":\"approve_pairing\"}";
    TEST_ASSERT_TRUE(parseCommand(ja, strlen(ja), c).ok);
    TEST_ASSERT_EQUAL(CmdKind::APPROVE_PAIRING, c.kind);

    const char *jbad = "{\"kind\":\"open\",\"zoneId\":0,\"durationS\":600}";
    TEST_ASSERT_FALSE(parseCommand(jbad, strlen(jbad), c).ok); // zoneId 0
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED.

- [ ] **Step 3: Declarar no header**

Em `IrrigationWebApi.h`, dentro do namespace:

```cpp
ParseResult parseProgramUpsert(const char *json, size_t len, Program &out);
ParseResult parseProgramToggle(const char *json, size_t len, uint8_t &outId, bool &outEnabled);
ParseResult parseProgramDelete(const char *json, size_t len, uint8_t &outId);

enum class CmdKind : uint8_t { NONE, PULSE_TEST, OPEN, CLOSE, ACK_ALERT, APPROVE_PAIRING };
struct WebCommand {
    CmdKind kind = CmdKind::NONE;
    uint8_t zoneId = 0;
    uint16_t durationS = 0;
    uint32_t node = 0;
};
ParseResult parseCommand(const char *json, size_t len, WebCommand &out);
```

- [ ] **Step 4: Implementar**

Em `IrrigationWebApi.cpp`. O array `steps` não é coberto pelo `JsonReader` plano; parsear com uma varredura dedicada dos objetos `{...}` dentro de `"steps":[...]`.

```cpp
ParseResult parseProgramUpsert(const char *json, size_t len, Program &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0, days = 0, start = 0;
    bool enabled = true;
    if (!rd.getInt("id", id) || id < 1 || id > 255) r.fail("id invalido (1..255)");
    if (!rd.getInt("daysMask", days) || days < 0 || days > 127) r.fail("daysMask fora de 0..127");
    if (!rd.getInt("startMinute", start) || start < 0 || start > 1439) r.fail("startMinute fora de 0..1439");
    rd.getBool("enabled", enabled); // opcional

    // Localiza o array "steps".
    const char *sp = strstr(json, "\"steps\"");
    const char *arr = sp ? strchr(sp, '[') : nullptr;
    const char *arrEnd = arr ? strchr(arr, ']') : nullptr;
    if (!arr || !arrEnd) r.fail("steps ausente");
    Program p = Program{};
    uint8_t count = 0;
    if (arr && arrEnd) {
        const char *o = arr;
        while ((o = strchr(o, '{')) != nullptr && o < arrEnd) {
            const char *oEnd = strchr(o, '}');
            if (!oEnd || oEnd > arrEnd) break;
            if (count >= 8) { r.fail("mais de 8 etapas"); break; }
            JsonReader sr(o, (size_t)(oEnd - o + 1));
            int64_t zoneId = 0, dur = 0;
            if (!sr.getInt("zoneId", zoneId) || zoneId < 1 || zoneId > 255) { r.fail("zoneId de etapa invalido"); break; }
            if (!sr.getInt("durationMin", dur) || dur < 1 || dur > 120) { r.fail("durationMin de etapa fora de 1..120"); break; }
            p.steps[count].zoneId = (uint8_t)zoneId;
            p.steps[count].durationMin = (uint16_t)dur;
            count++;
            o = oEnd + 1;
        }
    }
    if (count == 0) r.fail("programa sem etapas");
    if (!r.ok) return r;
    p.id = (uint8_t)id;
    p.enabled = enabled;
    p.daysMask = (uint8_t)days;
    p.startMinute = (uint16_t)start;
    p.stepCount = count;
    out = p;
    return r;
}

ParseResult parseProgramToggle(const char *json, size_t len, uint8_t &outId, bool &outEnabled)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    bool en = false;
    if (!rd.getInt("id", id) || id < 1 || id > 255) r.fail("id invalido (1..255)");
    if (!rd.getBool("enabled", en)) r.fail("enabled ausente");
    if (!r.ok) return r;
    outId = (uint8_t)id;
    outEnabled = en;
    return r;
}

ParseResult parseProgramDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > 255) { r.fail("id invalido (1..255)"); return r; }
    outId = (uint8_t)id;
    return r;
}

ParseResult parseCommand(const char *json, size_t len, WebCommand &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    char kind[20] = {0};
    if (!rd.getStr("kind", kind, sizeof(kind))) { r.fail("kind ausente"); return r; }
    WebCommand c = WebCommand{};
    int64_t zoneId = 0, dur = 0, node = 0;
    rd.getInt("zoneId", zoneId);
    rd.getInt("durationS", dur);
    rd.getInt("node", node);
    c.zoneId = (uint8_t)zoneId;
    c.node = (uint32_t)node;
    if (strcmp(kind, "pulse") == 0) {
        if (zoneId < 1 || zoneId > 255) r.fail("zoneId invalido");
        c.kind = CmdKind::PULSE_TEST;
        c.durationS = 10; // §7.1: teste de pulso abre 10 s
    } else if (strcmp(kind, "open") == 0) {
        if (zoneId < 1 || zoneId > 255) r.fail("zoneId invalido");
        if (dur < 1 || dur > 7200) r.fail("durationS fora de 1..7200");
        c.kind = CmdKind::OPEN;
        c.durationS = (uint16_t)dur;
    } else if (strcmp(kind, "close") == 0) {
        if (zoneId < 1 || zoneId > 255) r.fail("zoneId invalido");
        c.kind = CmdKind::CLOSE;
    } else if (strcmp(kind, "ack") == 0) {
        c.kind = CmdKind::ACK_ALERT; // node/zoneId identificam o alerta
    } else if (strcmp(kind, "approve_pairing") == 0) {
        c.kind = CmdKind::APPROVE_PAIRING;
    } else {
        r.fail("kind desconhecido");
    }
    if (!r.ok) return r;
    out = c;
    return r;
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, todos PASS.

- [ ] **Step 6: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): webapi program/command parsers with validation"
```

---

## Task 8: `StationTelemetryCache` + alimentar no heartbeat

O painel mostra vbat/vpainel/SNR/reboots/epoch por estação; hoje nada disso é guardado. Nova classe pura (como as outras tabelas), alimentada em `handleGwHeartbeat`.

**Files:**
- Create: `src/modules/irrigation/StationTelemetryCache.h`
- Create: `src/modules/irrigation/StationTelemetryCache.cpp`
- Modify: `src/modules/irrigation/IrrigationGateway.h`
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (dentro de `handleGwHeartbeat`)
- Test: `test/test_irrigation_gwtables/test_main.cpp`

**Interfaces:**
- Produces:
  - `struct StationTelemetry { uint32_t node; uint16_t vbatCentiV; uint16_t vpanelCentiV; int8_t snrQuarterDb; uint16_t rebootCount; uint8_t flags; uint32_t configEpoch; uint32_t atMs; };`
  - `class StationTelemetryCache { public: static constexpr size_t MAX = 16; void update(const StationTelemetry &t); const StationTelemetry *byNode(uint32_t node) const; };`
- Consumes (no módulo): `IrrigationProto::Heartbeat` (campos `vbatCentiV`, `vpanelCentiV`, `snrQuarterDb`, `rebootCount`, `flags`, `configEpoch`).

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_gwtables/test_main.cpp` (registrar no `main`; adicionar `#include "modules/irrigation/StationTelemetryCache.h"` no topo):

```cpp
static void test_telemetryCache_upsertAndLookup()
{
    StationTelemetryCache c;
    StationTelemetry t = {};
    t.node = 0x55; t.vbatCentiV = 1230; t.vpanelCentiV = 1810; t.snrQuarterDb = 24;
    t.rebootCount = 4; t.flags = 0x02; t.configEpoch = 9; t.atMs = 1000;
    c.update(t);
    const StationTelemetry *got = c.byNode(0x55);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_UINT16(1230, got->vbatCentiV);
    TEST_ASSERT_EQUAL_UINT32(9, got->configEpoch);
    // update do mesmo nó sobrescreve
    t.vbatCentiV = 1200; t.atMs = 2000;
    c.update(t);
    TEST_ASSERT_EQUAL_UINT16(1200, c.byNode(0x55)->vbatCentiV);
    TEST_ASSERT_NULL(c.byNode(0x99));
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_irrigation_gwtables`
Expected: RED — header inexistente.

- [ ] **Step 3: Criar a classe**

Criar `src/modules/irrigation/StationTelemetryCache.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

struct StationTelemetry {
    uint32_t node = 0; // 0 = slot vazio
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0;
    int8_t snrQuarterDb = 0;
    uint16_t rebootCount = 0;
    uint8_t flags = 0;
    uint32_t configEpoch = 0;
    uint32_t atMs = 0;
};

// Último heartbeat por estação (só RAM; não persiste — reconstrói ao ouvir de novo).
class StationTelemetryCache
{
  public:
    static constexpr size_t MAX = 16;
    void update(const StationTelemetry &t);          // upsert por node
    const StationTelemetry *byNode(uint32_t node) const;

  private:
    StationTelemetry entries[MAX] = {};
};
```

Criar `src/modules/irrigation/StationTelemetryCache.cpp`:

```cpp
#include "modules/irrigation/StationTelemetryCache.h"

void StationTelemetryCache::update(const StationTelemetry &t)
{
    if (t.node == 0) return;
    int free = -1, oldest = 0;
    uint32_t oldestMs = 0xFFFFFFFF;
    for (size_t i = 0; i < MAX; i++) {
        if (entries[i].node == t.node) { entries[i] = t; return; }
        if (entries[i].node == 0 && free < 0) free = (int)i;
        if (entries[i].atMs < oldestMs) { oldestMs = entries[i].atMs; oldest = (int)i; }
    }
    entries[(free >= 0) ? (size_t)free : (size_t)oldest] = t; // recicla o mais antigo se cheio
}

const StationTelemetry *StationTelemetryCache::byNode(uint32_t node) const
{
    for (size_t i = 0; i < MAX; i++)
        if (entries[i].node == node)
            return &entries[i];
    return nullptr;
}
```

- [ ] **Step 4: Adicionar ao agregado + alimentar no heartbeat**

Em `src/modules/irrigation/IrrigationGateway.h`, adicionar o include e o membro:

```cpp
#include "modules/irrigation/StationTelemetryCache.h"
```
```cpp
    StationTelemetryCache telemetry;
```

Em `src/modules/irrigation/IrrigationModule.cpp`, dentro de `handleGwHeartbeat` (após decodificar o `Heartbeat hb` e identificar `from`), inserir:

```cpp
    StationTelemetry tel = {};
    tel.node = from;
    tel.vbatCentiV = hb.vbatCentiV;
    tel.vpanelCentiV = hb.vpanelCentiV;
    tel.snrQuarterDb = hb.snrQuarterDb;
    tel.rebootCount = hb.rebootCount;
    tel.flags = hb.flags;
    tel.configEpoch = hb.configEpoch;
    tel.atMs = millis();
    gateway.telemetry.update(tel);
```

(Se os nomes locais em `handleGwHeartbeat` diferirem — ex.: variável de heartbeat ou do remetente —, ajustar para os nomes reais lidos no início do handler. Ler o handler antes de editar.)

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_gwtables`
Expected: FILTERED, PASS.

- [ ] **Step 6: Rodar suíte completa**

Run: `./bin/run-tests.sh`
Expected: `RESULT: GREEN 44/44 suites passed`.

- [ ] **Step 7: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/StationTelemetryCache.h src/modules/irrigation/StationTelemetryCache.cpp src/modules/irrigation/IrrigationGateway.h src/modules/irrigation/IrrigationModule.cpp test/test_irrigation_gwtables/test_main.cpp
git add -A
git commit -m "feat(irrigation): per-station telemetry cache fed from heartbeat"
```

---

## Task 9: Métodos de serviço do gateway no `IrrigationModule`

Ponte entre a cola HTTP (Task 10) e o estado do gateway: aplicar edições (persistindo) e despachar comandos (via caminho existente com tracker/epoch), além de expor leitura para os serializadores. Sem WiFi/HTTP — compila no build nativo.

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`
- Modify: `src/modules/irrigation/IrrigationModule.cpp`

**Interfaces:**
- Consumes: `Zone`, `Program`, `IrrigationWeb::WebCommand`, `IrrigationGateway`.
- Produces (métodos públicos em `IrrigationModule`):
  - `bool gwIsGateway() const;` — `settings.role == (uint8_t)IrrigationRole::GATEWAY`.
  - `const IrrigationGateway &gwState() const;`
  - `bool gwHasRtc() const;` / `uint32_t gwLocalSecs() const;` — usa a mesma fonte de hora do `gwTick` (extrair helper se necessário).
  - `bool gwApplyZoneUpsert(const Zone &z);` — `gateway.zones.upsert(z)` + `saveGatewayState()`. `false` se tabela cheia.
  - `bool gwApplyZoneDelete(uint8_t id);` — `removeById` + persist.
  - `bool gwApplyProgramUpsert(const Program &p);` — `gateway.scheduler.upsert(p)` + persist.
  - `bool gwApplyProgramToggle(uint8_t id, bool enabled);` — carrega o programa via `programAt`/busca, ajusta `enabled`, re-`upsert`, persist.
  - `bool gwApplyProgramDelete(uint8_t id);` — `scheduler.removeById(id)` + persist; se era o programa em execução, `scheduler.abort()` e fecha a zona corrente.
  - `bool gwRunCommand(const IrrigationWeb::WebCommand &c);` — despacha: `PULSE_TEST`/`OPEN`/`CLOSE` → resolve zona por `c.zoneId`, chama `gwSendValveCmd(z->node, z->index, z->tipo, action, durationS, z->id, attempts)`; `ACK_ALERT` → marca alerta reconhecido (ver nota); `APPROVE_PAIRING` → `commitPairing()`.

Nota ACK_ALERT: `AlertCenter` é um ring sem "reconhecido". Para 5a, "reconhecer" = registrar em `IrrigationModule` o `atMs` do último alerta reconhecido por nó e o serializador de overview conta apenas alertas com `atMs` > esse marcador. Implementação mínima: um `uint32_t lastAckAllMs` global; `ACK_ALERT` seta `lastAckAllMs = millis()`; `OverviewCtx.alertCount` passa a contar alertas com `atMs > lastAckAllMs`. (Reconhecimento por-alerta fica para a Fase 6, junto do log.)

- [ ] **Step 1: Declarar os métodos no header**

Em `src/modules/irrigation/IrrigationModule.h`, na seção `public:`:

```cpp
    // --- Serviço do painel web (gateway). Chamados pela cola HTTP (IrrigationWebEndpoints). ---
    bool gwIsGateway() const;
    const IrrigationGateway &gwState() const { return gateway; }
    bool gwHasRtc() const;
    uint32_t gwLocalSecs() const;
    bool gwApplyZoneUpsert(const Zone &z);
    bool gwApplyZoneDelete(uint8_t id);
    bool gwApplyProgramUpsert(const Program &p);
    bool gwApplyProgramToggle(uint8_t id, bool enabled);
    bool gwApplyProgramDelete(uint8_t id);
    bool gwRunCommand(const struct IrrigationWeb::WebCommand &c);
```

Adicionar, junto aos outros membros privados: `uint32_t lastAckAllMs = 0;`
Adicionar o include no topo do `.cpp` (não do header, p/ não poluir): em `IrrigationModule.cpp`, `#include "modules/irrigation/IrrigationWebApi.h"`.
No header, para o forward-declare funcionar, adicionar antes da classe: `namespace IrrigationWeb { struct WebCommand; }`.

- [ ] **Step 2: Implementar os métodos**

Em `src/modules/irrigation/IrrigationModule.cpp` (usar a mesma lógica de hora local do `gwTick`; se lá a hora vem de uma variável/known-good local, extrair um helper `bool computeLocalSecs(uint32_t &out) const` e usá-lo tanto no `gwTick` quanto em `gwHasRtc`/`gwLocalSecs`):

```cpp
bool IrrigationModule::gwIsGateway() const { return settings.role == (uint8_t)IrrigationRole::GATEWAY; }

bool IrrigationModule::gwHasRtc() const
{
    uint32_t s = 0;
    return computeLocalSecs(s);
}
uint32_t IrrigationModule::gwLocalSecs() const
{
    uint32_t s = 0;
    computeLocalSecs(s);
    return s;
}

bool IrrigationModule::gwApplyZoneUpsert(const Zone &z)
{
    if (!gateway.zones.upsert(z)) return false;
    saveGatewayState();
    return true;
}
bool IrrigationModule::gwApplyZoneDelete(uint8_t id)
{
    bool ok = gateway.zones.removeById(id);
    if (ok) saveGatewayState();
    return ok;
}
bool IrrigationModule::gwApplyProgramUpsert(const Program &p)
{
    if (!gateway.scheduler.upsert(p)) return false;
    saveGatewayState();
    return true;
}
bool IrrigationModule::gwApplyProgramToggle(uint8_t id, bool enabled)
{
    // localiza o programa atual e re-upsert com enabled ajustado
    for (size_t i = 0; i < gateway.scheduler.count(); i++) {
        const Program *cur = gateway.scheduler.programAt(i);
        if (cur && cur->id == id) {
            Program np = *cur;
            np.enabled = enabled;
            gateway.scheduler.upsert(np);
            saveGatewayState();
            return true;
        }
    }
    return false;
}
bool IrrigationModule::gwApplyProgramDelete(uint8_t id)
{
    if (gateway.scheduler.running() && gateway.scheduler.currentZone() != 0) {
        // fecha a zona corrente antes de abortar (mesma disciplina do gwTick)
        const Zone *z = gateway.zones.byId(gateway.scheduler.currentZone());
        gateway.scheduler.abort();
        if (z) gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
    }
    bool ok = gateway.scheduler.removeById(id);
    if (ok) saveGatewayState();
    return ok;
}
bool IrrigationModule::gwRunCommand(const IrrigationWeb::WebCommand &c)
{
    using K = IrrigationWeb::CmdKind;
    if (c.kind == K::APPROVE_PAIRING) { commitPairing(); return true; }
    if (c.kind == K::ACK_ALERT) { lastAckAllMs = millis(); return true; }
    const Zone *z = gateway.zones.byId(c.zoneId);
    if (!z) return false;
    const StationEntry *st = gateway.stations.byNode(z->node);
    uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
    if (c.kind == K::OPEN || c.kind == K::PULSE_TEST) {
        uint16_t dur = c.durationS;
        if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60)) dur = (uint16_t)(z->maxMin * 60);
        gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts);
        return true;
    }
    if (c.kind == K::CLOSE) {
        gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
        return true;
    }
    return false;
}
```

Refatorar `gwTick`: onde ele computa `epochLocal` e decide se há RTC, substituir por `uint32_t epochLocal = 0; bool hasRtc = computeLocalSecs(epochLocal);` e o ramo `if (!hasRtc) { ...warn... } else { ...drena scheduler... }`. Declarar `bool computeLocalSecs(uint32_t &out) const;` como método privado no header e mover para lá a lógica que hoje está inline no `gwTick`.

- [ ] **Step 3: Rodar suíte completa (garantir que nada quebrou)**

Run: `./bin/run-tests.sh`
Expected: `RESULT: GREEN 44/44 suites passed`.

- [ ] **Step 4: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): gateway web service methods (apply/dispatch/read) on module"
```

---

## Task 10: Cola HTTP — `IrrigationWebEndpoints` (só ESP32 / CI)

Registra as rotas `/api/irrigation/*` no `HTTPServer` do Meshtastic e liga parse→aplicar→responder. Não roda no build nativo (guardado). Verificação = build de CI + suíte nativa continua GREEN.

**Files:**
- Create: `src/modules/irrigation/IrrigationWebEndpoints.h`
- Create: `src/modules/irrigation/IrrigationWebEndpoints.cpp`
- Modify: `src/mesh/http/ContentHandler.cpp` (uma chamada em `registerHandlers`)

**Interfaces:**
- Produces: `void registerIrrigationHandlers(httpsserver::HTTPServer *server);` (declarada no `.h`, guardada por `#if !MESHTASTIC_EXCLUDE_WEBSERVER`).
- Consumes: `irrigationModule->gwIsGateway()`, `gwState()`, `gwHasRtc()`, `gwApply*`, `gwRunCommand`; `IrrigationWeb::build*`/`parse*`.

Rotas (todas checam `irrigationModule && irrigationModule->gwIsGateway()`; senão respondem 404):

| Método | Caminho | Ação |
|---|---|---|
| GET | `/api/irrigation/overview` | monta `OverviewCtx` do `gwState()` + `gwHasRtc()` → `buildOverview` |
| GET | `/api/irrigation/stations` | monta `StationView[]` de `gwState().stations.nodeAt(i)` + `telemetry.byNode` + `monitor.lastHeardMs` + `computeSync` → `buildStations` |
| GET | `/api/irrigation/zones` | `buildZones(gwState().zones)` |
| POST | `/api/irrigation/zones` | `parseZoneUpsert` → `gwApplyZoneUpsert`; 200 view ou 400 erros |
| POST | `/api/irrigation/zones/delete` | `parseZoneDelete` → `gwApplyZoneDelete` |
| GET | `/api/irrigation/programs` | `buildPrograms(gwState().scheduler)` |
| POST | `/api/irrigation/programs` | `parseProgramUpsert` → `gwApplyProgramUpsert` |
| POST | `/api/irrigation/programs/toggle` | `parseProgramToggle` → `gwApplyProgramToggle` |
| POST | `/api/irrigation/programs/delete` | `parseProgramDelete` → `gwApplyProgramDelete` |
| POST | `/api/irrigation/command` | `parseCommand` → `gwRunCommand` |

- [ ] **Step 1: Criar o header**

Criar `src/modules/irrigation/IrrigationWebEndpoints.h`:

```cpp
#pragma once
#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include <HTTPServer.hpp>
void registerIrrigationHandlers(httpsserver::HTTPServer *server);
#endif
```

- [ ] **Step 2: Criar o .cpp**

Criar `src/modules/irrigation/IrrigationWebEndpoints.cpp`. Seguir **exatamente** o padrão de `src/mesh/http/ContentHandler.cpp` (`ResourceNode`, `HTTPRequest`, `HTTPResponse`, leitura de corpo como em `handleAPIv1ToRadio` em `ContentHandler.cpp:201`, e `res->setHeader`/`res->print`). Estrutura:

```cpp
#include "modules/irrigation/IrrigationWebEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/IrrigationWebApi.h"
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <ResourceNode.hpp>

using namespace httpsserver;
using namespace IrrigationWeb;

static bool gwReady() { return irrigationModule && irrigationModule->gwIsGateway(); }

static void sendJson(HTTPResponse *res, const char *body, int status = 200)
{
    res->setStatusCode(status);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->print(body);
}

static size_t readBody(HTTPRequest *req, char *buf, size_t cap)
{
    size_t n = 0;
    while (!req->requestComplete() && n + 1 < cap) {
        n += req->readChars(buf + n, cap - 1 - n);
    }
    buf[n] = '\0';
    return n;
}

// --- GET handlers ---
static void hOverview(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    const IrrigationGateway &g = irrigationModule->gwState();
    OverviewCtx c = {};
    c.hasRtc = irrigationModule->gwHasRtc();
    c.stationCount = (uint16_t)g.stations.count();
    c.running = g.scheduler.running() ? 1 : 0;
    c.runningZoneId = g.scheduler.currentZone();
    // c.alertCount / pairing preenchidos conforme estado disponível no módulo.
    char buf[512];
    if (!buildOverview(c, buf, sizeof(buf))) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// hStations, hZonesGet, hProgramsGet — análogos, usando build* e montando as views.

// --- POST handlers (parse -> apply -> responder) ---
static void hZonesPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[512];
    size_t n = readBody(req, body, sizeof(body));
    Zone z;
    ParseResult pr = parseZoneUpsert(body, n, z);
    if (!pr.ok) {
        char err[256];
        JsonWriter w(err, sizeof(err));
        w.beginObject(); w.key("errors"); w.beginArray();
        for (uint8_t i = 0; i < pr.errorCount; i++) w.str(pr.errors[i].msg);
        w.endArray(); w.endObject(); w.done();
        sendJson(res, err, 400);
        return;
    }
    if (!irrigationModule->gwApplyZoneUpsert(z)) { sendJson(res, "{\"errors\":[\"tabela cheia\"]}", 400); return; }
    char out[1024];
    buildZones(irrigationModule->gwState().zones, out, sizeof(out));
    sendJson(res, out);
}

// hZonesDelete, hProgramsPost, hProgramsToggle, hProgramsDelete, hCommand — mesmo formato.

void registerIrrigationHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/irrigation/overview", "GET", &hOverview));
    server->registerNode(new ResourceNode("/api/irrigation/stations", "GET", &hStations));
    server->registerNode(new ResourceNode("/api/irrigation/zones", "GET", &hZonesGet));
    server->registerNode(new ResourceNode("/api/irrigation/zones", "POST", &hZonesPost));
    server->registerNode(new ResourceNode("/api/irrigation/zones/delete", "POST", &hZonesDelete));
    server->registerNode(new ResourceNode("/api/irrigation/programs", "GET", &hProgramsGet));
    server->registerNode(new ResourceNode("/api/irrigation/programs", "POST", &hProgramsPost));
    server->registerNode(new ResourceNode("/api/irrigation/programs/toggle", "POST", &hProgramsToggle));
    server->registerNode(new ResourceNode("/api/irrigation/programs/delete", "POST", &hProgramsDelete));
    server->registerNode(new ResourceNode("/api/irrigation/command", "POST", &hCommand));
}
#endif
```

Implementar os handlers omitidos (`hStations`, `hZonesGet`, `hProgramsGet`, `hZonesDelete`, `hProgramsPost`, `hProgramsToggle`, `hProgramsDelete`, `hCommand`) seguindo o mesmo esqueleto: GET → `build*`; POST → `parse*` → `gwApply*`/`gwRunCommand` → responder view ou `{"errors":[...]}` 400. Para `hStations`, montar o `StationView[]` iterando `g.stations.count()`/`nodeAt(i)`, cruzando com `g.telemetry.byNode(node)` (vbat/vpanel/snr/reboot/flags/epoch), `g.monitor.lastHeardMs(node)` (→ `secsSinceHeard = (millis()-lastHeard)/1000`), e `computeSync(entry->desiredEpoch, tel?tel->configEpoch:0, silent)` onde `silent` = `secsSinceHeard*1000 > entry->silencioAlertaMin*60000`. Confirmar a assinatura real de leitura de corpo do `esp32_https_server` lendo `ContentHandler.cpp:201-230` antes de implementar `readBody`.

- [ ] **Step 3: Ligar em `ContentHandler::registerHandlers`**

Em `src/mesh/http/ContentHandler.cpp`, no topo adicionar:

```cpp
#include "modules/irrigation/IrrigationWebEndpoints.h"
```

Dentro de `registerHandlers(HTTPServer *insecureServer, HTTPSServer *secureServer)`, ao final do corpo (registrar nos dois servidores como o restante faz), adicionar:

```cpp
    registerIrrigationHandlers(insecureServer);
    registerIrrigationHandlers(secureServer);
```

(Seguir o mesmo padrão de dupla-registração que os nós `/api/v1/*` já usam neste arquivo; se o arquivo registra cada nó em ambos, replicar.)

- [ ] **Step 4: Verificar que o build nativo continua GREEN (cola excluída)**

Run: `./bin/run-tests.sh`
Expected: `RESULT: GREEN 44/44 suites passed` (o `.cpp` é compilado, mas todo o corpo está sob `#if !MESHTASTIC_EXCLUDE_WEBSERVER`, que está definido no env nativo → vira TU vazia).

- [ ] **Step 5: Commitar (build ESP32 fica p/ CI)**

```bash
trunk fmt src/modules/irrigation/IrrigationWebEndpoints.h src/modules/irrigation/IrrigationWebEndpoints.cpp src/mesh/http/ContentHandler.cpp
git add src/modules/irrigation/IrrigationWebEndpoints.h src/modules/irrigation/IrrigationWebEndpoints.cpp src/mesh/http/ContentHandler.cpp
git commit -m "feat(irrigation): http endpoints /api/irrigation/* (gateway-gated, esp32-only)"
```

> **CI é obrigatório aqui:** o build tbeam/gateway ESP32 nunca roda localmente. Após o push, confirmar que o job de build ESP32 compila `IrrigationWebEndpoints.cpp` contra o `esp32_https_server` real. Ajustar nomes de API (`readChars`/`requestComplete`/`registerNode`) conforme os erros do CI, se houver.

---

## Task 11: Frontend — shell + Visão Geral + Estações

Assets estáticos em LittleFS. Não há teste nativo; verificação é no navegador (após CI flashar a imagem). Reproduzir a linguagem visual do mockup `myfork/Irrigacao Mobile.dc.html` (paleta oklch, cards, chips, barra de abas inferior). Este task entrega o shell navegável + as duas telas de leitura.

**Files:**
- Create: `data/irrigacao/index.html`
- Create: `data/irrigacao/style.css`
- Create: `data/irrigacao/app.js`

**Referência de layout:** mockup, telas: Visão Geral (`Irrigacao Mobile.dc.html:46-115`), Estações (`:117-169`). Tokens visuais: fundo `oklch(0.985 0.004 100)`, verde primário `oklch(0.47 0.1 150)`, âmbar `oklch(0.65 0.15 75)`, vermelho `oklch(0.55 0.16 30)`, texto `oklch(0.22 0.008 100)`, cinza `oklch(0.52 0.006 100)`, card branco com borda `oklch(0.9 0.006 100)` e raio 14-16px.

**Contrato dos endpoints consumidos:** `GET /api/irrigation/overview` → objeto de Task 3; `GET /api/irrigation/stations` → array de Task 4.

- [ ] **Step 1: HTML shell + barra de abas**

Criar `data/irrigacao/index.html` com: cabeçalho ("Fazenda" + chip de sincronização), `<main id="view">` (preenchido por JS), e barra inferior com abas **Visão Geral · Estações · Zonas · Programas** (as duas últimas ligadas nas Tasks 12/13; Grupos/Log/Sistema como itens desabilitados "em breve"). Carregar `style.css` e `app.js`.

```html
<!DOCTYPE html><html lang="pt-BR"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Irrigação — Gateway</title><link rel="stylesheet" href="style.css"></head>
<body>
<header class="top"><div><div class="farm">Gateway</div><div class="sub">canal privado</div></div>
<span id="syncChip" class="chip green">—</span></header>
<main id="view" class="view"></main>
<nav class="tabs">
  <button data-tab="overview" class="tab active">Visão&nbsp;Geral</button>
  <button data-tab="stations" class="tab">Estações</button>
  <button data-tab="zones" class="tab">Zonas</button>
  <button data-tab="programs" class="tab">Programas</button>
</nav>
<script src="app.js"></script></body></html>
```

- [ ] **Step 2: CSS com os tokens do mockup**

Criar `data/irrigacao/style.css` reproduzindo os tokens acima: `:root` com as variáveis de cor, `.top`, `.chip.green/.amber/.red`, `.card`, `.tabs`/`.tab.active`, tipografia system-ui. (Extrair espaçamentos/raios do mockup; alvo mobile 390px, responsivo.)

- [ ] **Step 3: JS — roteador de abas + fetch/render de Visão Geral e Estações**

Criar `data/irrigacao/app.js`:

```js
const API = '/api/irrigation';
const view = document.getElementById('view');
const syncChip = document.getElementById('syncChip');
let current = 'overview';
let timer = null;

async function getJson(path){ const r = await fetch(API+path); if(!r.ok) throw new Error(r.status); return r.json(); }
async function postJson(path, body){ const r = await fetch(API+path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const j = await r.json().catch(()=>({})); return {ok:r.ok, body:j}; }

function fmtSince(s){ if(s<60) return 'há '+s+' s'; if(s<3600) return 'há '+Math.floor(s/60)+' min'; return 'há '+Math.floor(s/3600)+' h'; }

async function renderOverview(){
  const o = await getJson('/overview');
  syncChip.textContent = o.hasRtc ? 'com relógio' : 'sem relógio';
  syncChip.className = 'chip ' + (o.hasRtc ? 'green':'amber');
  view.innerHTML = `
    <div class="row3">
      <div class="card stat"><div class="lbl">Estações</div><div class="val">${o.stationCount}</div></div>
      <div class="card stat"><div class="lbl">Em execução</div><div class="val">${o.running?('zona '+o.runningZoneId):'—'}</div></div>
      <div class="card stat"><div class="lbl">Alertas</div><div class="val warn">${o.alertCount}</div></div>
    </div>
    ${o.pairingPending?`<div class="card amberbox">Pareamento pendente — expira em ${o.pairingSecondsLeft}s</div>`:''}
    ${!o.hasRtc?`<div class="card amberbox">Sem relógio — cronograma inativo. Modo espelho segue operando.</div>`:''}`;
}

async function renderStations(){
  const list = await getJson('/stations');
  view.innerHTML = list.map(s=>`
    <div class="card station">
      <div class="name">${s.name||('0x'+s.node.toString(16))}</div>
      <div class="sub">${fmtSince(s.secsSinceHeard)} · ${(s.vbatCentiV/100).toFixed(1)} V</div>
      <div class="chips">
        <span class="chip ${s.sync==='sincronizada'?'green':s.sync==='pendente'?'amber':'red'}">${s.sync}</span>
      </div>
    </div>`).join('') || '<div class="empty">Nenhuma estação registrada.</div>';
}

const RENDER = { overview: renderOverview, stations: renderStations };
// zones/programs adicionados nas Tasks 12/13.

async function show(tab){
  current = tab;
  document.querySelectorAll('.tab').forEach(t=>t.classList.toggle('active', t.dataset.tab===tab));
  if(timer) clearInterval(timer);
  const fn = RENDER[tab];
  if(!fn){ view.innerHTML='<div class="empty">Em breve.</div>'; return; }
  try{ await fn(); }catch(e){ view.innerHTML='<div class="empty">Erro ao carregar ('+e.message+').</div>'; }
  if(tab==='overview'||tab==='stations') timer = setInterval(()=>fn().catch(()=>{}), 3000);
}

document.querySelectorAll('.tab').forEach(t=>t.addEventListener('click',()=>show(t.dataset.tab)));
show('overview');
```

- [ ] **Step 4: Commitar (verificação em navegador via CI/hardware)**

```bash
trunk fmt data/irrigacao/index.html data/irrigacao/style.css data/irrigacao/app.js
git add data/irrigacao/index.html data/irrigacao/style.css data/irrigacao/app.js
git commit -m "feat(irrigation): panel frontend shell + overview + stations screens"
```

> **Verificação:** após a imagem LittleFS ir para hardware/CI, abrir `http://<gateway>/irrigacao/` e conferir que Visão Geral e Estações renderizam do endpoint. Sem harness nativo p/ isso.

---

## Task 12: Frontend — Zonas (lista + edição)

Tela de zonas com dropdowns nó→saída, durações, tipo, fonte física; POST para os endpoints.

**Files:**
- Modify: `data/irrigacao/app.js`
- Modify: `data/irrigacao/style.css` (estilos de formulário, se necessário)

**Referência de layout:** mockup Zonas lista (`Irrigacao Mobile.dc.html:171-206`) e edição (`:208-296`).

**Contrato:** `GET /api/irrigation/zones` → array de Task 5; `GET /api/irrigation/stations` (para o dropdown de nós); `POST /api/irrigation/zones` (corpo = campos de `parseZoneUpsert`, Task 6); `POST /api/irrigation/zones/delete` (`{id}`).

- [ ] **Step 1: `renderZones` (lista) + botão "Nova zona"**

Adicionar em `app.js` `renderZones()` que faz `getJson('/zones')` e monta cards com nome, `estação · saída · tipo`, e botões Editar/Abrir/Fechar (Abrir/Fechar via `POST /command` `{kind:'open'|'close',zoneId,durationS}`). Registrar em `RENDER.zones`.

- [ ] **Step 2: `zoneEditForm` (nova/editar)**

Formulário com: nome (input), tipo (Válvula/GPO), estação (chips das estações de `/stations`), saída física (índice 0..7), durações padrão/failsafe. Botão Salvar → `POST /zones`; se resposta 400, exibir `body.errors`. Botão Excluir → `POST /zones/delete`.

- [ ] **Step 3: Commitar**

```bash
trunk fmt data/irrigacao/app.js data/irrigacao/style.css
git add data/irrigacao/app.js data/irrigacao/style.css
git commit -m "feat(irrigation): panel zones list + edit"
```

> **Verificação:** navegador — criar/editar/excluir zona e ver refletir; erro de validação aparece na barra de erros.

---

## Task 13: Frontend — Programas (lista + edição)

Cronograma: dias, horário, sequência de zonas × duração; ativar/pausar.

**Files:**
- Modify: `data/irrigacao/app.js`
- Modify: `data/irrigacao/style.css` (se necessário)

**Referência de layout:** mockup Programas lista (`Irrigacao Mobile.dc.html:298-321`) e edição (`:323-400`).

**Contrato:** `GET /api/irrigation/programs` → array de Task 5; `GET /api/irrigation/zones` (para escolher zonas nas etapas); `POST /api/irrigation/programs` (corpo = `parseProgramUpsert`, Task 7); `POST /api/irrigation/programs/toggle` (`{id,enabled}`); `POST /api/irrigation/programs/delete` (`{id}`).

- [ ] **Step 1: `renderPrograms` (lista) + toggle ativo/pausado**

`getJson('/programs')` → cards com `dias · horário`, resumo da sequência, botão Ativo/Pausado (`POST /programs/toggle`). Registrar em `RENDER.programs`. Botão "Novo programa".

- [ ] **Step 2: `programEditForm`**

Chips de dias (dom..sáb → bits de `daysMask`), input `type=time` (→ `startMinute`), lista de etapas (cada uma: escolher zona das de `/zones` + duração em min), adicionar/remover etapa. Salvar → `POST /programs` (validação: 1..8 etapas, durações 1..120). Erros → barra de erros. Excluir → `POST /programs/delete`.

- [ ] **Step 3: Commitar**

```bash
trunk fmt data/irrigacao/app.js data/irrigacao/style.css
git add data/irrigacao/app.js data/irrigacao/style.css
git commit -m "feat(irrigation): panel programs list + edit"
```

> **Verificação:** navegador — criar programa com 2 etapas, pausar/ativar, excluir; sem RTC, a lista ainda renderiza (só não dispara).

---

## Self-Review (feito pelo autor do plano)

**Cobertura da spec (5a):**
- §7.1 Estações (registro/estado/sync/coordenadas) → Tasks 4, 8, 10 (GET stations), 11.
- §7.1 Zonas (roteamento nó→saída, durações, tipo, fonte) → Tasks 5, 6, 9, 10, 12.
- §7.1 Programas (dias/horário/sequência) → Tasks 5, 7, 9, 10, 13.
- §7.1 convergência por ACK/epoch → `computeSync` (Task 3) + campo `sync` (Task 4) exibido (Task 11).
- LittleFS + endpoints JSON → Tasks 10 (endpoints) + 11-13 (estáticos).
- Restrição sem-RTC → Task 9 (`gwHasRtc`) + Task 11 (banner).
- Carryover F4 (#1 suppress CLOSE, #2 nodeAt, #3 static_assert) → Tasks 1, 2.
- Adiado explicitamente (F6/F8/5b): Grupos & Intertravamentos, Log, Sistema/PIN/backup, captive portal — não implementados; abas desabilitadas (Task 11).

**Placeholders:** nenhum passo de código sem código; comandos e saídas esperadas presentes. Tasks de frontend (11-13) são build-and-verify (sem teste unitário — não há harness nativo de navegador), o que é explícito.

**Consistência de tipos:** `SyncState`/`syncLabel`, `OverviewCtx`, `StationView`, `ParseResult`/`ParseError`, `WebCommand`/`CmdKind`, `JsonWriter`/`JsonReader`, `zoneAt`/`programAt`/`nodeAt`, `StationTelemetry(Cache)`, `gwState`/`gwApply*`/`gwRunCommand`/`gwHasRtc`/`gwLocalSecs`/`computeLocalSecs` — nomes idênticos entre as tasks que os definem e as que os consomem.

**Pontos a confirmar durante a execução (não bloqueiam o plano):**
- Nomes reais das variáveis em `handleGwHeartbeat` (Task 8, Step 4) — ler o handler antes de editar.
- Como `gwTick` obtém hora local hoje (Task 9) — extrair `computeLocalSecs` da lógica existente.
- API exata de leitura de corpo/registro de nó do `esp32_https_server` (Task 10) — espelhar `ContentHandler.cpp:201`.
