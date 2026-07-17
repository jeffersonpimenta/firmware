# Fase 5b — Captive portal do nó — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Servir, em qualquer nó de irrigação, um captive portal de campo (AP Wi-Fi + DNS cativo levantado pelo botão) com abas **Este nó** (estado vivo + teste de pulso) e **Rede** (comandar a fazenda por rádio via relay no gateway), alimentado por endpoints JSON `/api/portal/*` e mantendo toda a lógica pura testada nativamente.

**Architecture:** Três camadas, espelhando a Fase 5a. (A) Núcleo puro C++ testado no host — `PortalSession` (máquina de estados do ciclo de vida do AP), extensões de `PortalApi` (build/parse JSON reusando o `JsonWriter`/`JsonReader` de `IrrigationWebApi`), e o codec `MSG_REMOTE_CMD`. (B) Cola só-ESP32 validada por CI — `PortalAp` (softAP/DNS/mDNS dirigido pela SM) e `IrrigationPortalEndpoints` (rotas `/api/portal/*`). (C) Frontend estático em LittleFS.

**Tech Stack:** C++17, PlatformIO Unity (suíte nativa `coverage`), `esp32_https_server`, `DNSServer`/`WiFi` do Arduino-ESP32, LittleFS, HTML/CSS/vanilla-JS.

---

## Global Constraints

- Teto absoluto de abertura **120 min** compilado (`ValveController::MAX_OPEN_SECONDS`); portal nunca o contorna.
- Fail-safe local sempre; versão de protocolo (`IrrigationProto::VERSION`) em toda mensagem; mismatch = rejeição segura.
- Payload de rádio ≤ ~200 bytes (`IrrigationProto::MAX_PAYLOAD`).
- Portal disponível em **todos os papéis**; o relay `MSG_REMOTE_CMD` só é **tratado** no gateway.
- Autorização = AP WPA2 + PIN (spec §7); sem token por-comando.
- Camada A é **pura**: só pode incluir `GatewayTables.h`, `ProgramScheduler.h`, `IrrigationWebApi.h`, `IrrigationProtocol.h`, `IrrigationSettings.h` e headers `<cstdint>`/`<cstddef>`/`<cstring>`/`<cstdio>`. **Nunca** `Arduino.h`, WiFi, HTTP, `FSCom`.
- Cola HTTP/AP (`IrrigationPortalEndpoints.cpp`, `PortalAp.cpp`) sob `#if !MESHTASTIC_EXCLUDE_WEBSERVER`; excluída do build nativo via `variants/native/portduino.ini` src_filter (mesmo mecanismo de `IrrigationWebEndpoints.cpp`).
- Testes: `./bin/run-tests.sh` GREEN. Cada suíte nova bumpa `test/native-suite-count` no MESMO commit (senão AMBER).
- Formatar com `trunk fmt` antes de cada commit (rodado em CI — não instalado localmente).
- Idioma: comentários em português; identificadores como nos arquivos vizinhos.

## Como rodar uma suíte nativa (esta máquina precisa de Docker)

Esta box não roda a suíte nativa direto (sem WSL Linux). Use o container do repo (PowerShell):

```powershell
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f <suite>
```

(Pré-req uma vez: `docker build -f Dockerfile.test -t mesh-test .`.) Uma corrida filtrada compila o firmware inteiro, então também valida os edits de módulo/cola (erros de link aparecem). Exit codes: 0 GREEN, 1 RED, 2 AMBER, 3 FILTERED.

## File Structure

Criar:

- `src/modules/irrigation/PortalSession.h` / `.cpp` — máquina de estados do ciclo de vida do AP (puro).
- `src/modules/irrigation/PortalApi.h` / `.cpp` — build/parse JSON das abas do portal (puro; reusa `IrrigationWeb`).
- `src/modules/irrigation/PortalAp.h` / `.cpp` — cola só-ESP32 do AP (softAP/DNS/mDNS).
- `src/modules/irrigation/IrrigationPortalEndpoints.h` / `.cpp` — cola HTTP só-ESP32 (`/api/portal/*`).
- `test/test_portal_session/test_main.cpp` — suíte nativa da SM.
- `test/test_portal_api/test_main.cpp` — suíte nativa do PortalApi.
- `data/irrigacao/portal/index.html`, `data/irrigacao/portal/app.js` — frontend do portal.

Modificar:

- `src/modules/irrigation/IrrigationProtocol.h` / `.cpp` — `MSG_REMOTE_CMD`, `RemoteCmd`, encode/decode (Task 8).
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — membro `PortalSession`, métodos de serviço do portal, handler `MSG_REMOTE_CMD`, hook do botão (Tasks 3, 6, 10).
- `src/mesh/http/ContentHandler.cpp` — 1 chamada a `registerIrrigationPortalHandlers` (Task 5).
- `variants/native/portduino.ini` — excluir as duas colas novas do build nativo (Tasks 4, 5).
- `test/test_irrigation_protocol/test_main.cpp` — round-trip de `MSG_REMOTE_CMD` (Task 8).
- `test/native-suite-count` — 44 → 45 (Task 1) → 46 (Task 2).

---

# MARCO 1 — AP shell + aba "Este nó"

## Task 1: `PortalSession` — máquina de estados do ciclo de vida do AP

Modela o AP como timer puro: `requestOpen` (botão) abre por 10 min; cada tick com cliente presente renova; sem cliente por 10 min → fecha (spec §7.2, §8.7). Sem Arduino/WiFi — testável no host.

**Files:**
- Create: `src/modules/irrigation/PortalSession.h`
- Create: `src/modules/irrigation/PortalSession.cpp`
- Create: `test/test_portal_session/test_main.cpp`
- Modify: `test/native-suite-count`

- [ ] **Step 1: Escrever o teste que falha**

`test/test_portal_session/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/PortalSession.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static const uint32_t TEN_MIN = 10u * 60u * 1000u;

static void test_startsClosed()
{
    PortalSession s;
    TEST_ASSERT_FALSE(s.apShouldBeUp());
    TEST_ASSERT_EQUAL_UINT32(0, s.secondsLeft(0));
}

static void test_requestOpen_raisesAp()
{
    PortalSession s;
    s.requestOpen(1000);
    TEST_ASSERT_TRUE(s.apShouldBeUp());
    TEST_ASSERT_EQUAL_UINT32(600, s.secondsLeft(1000)); // 10 min
}

static void test_idleTimeout_closes()
{
    PortalSession s;
    s.requestOpen(0);
    s.tick(TEN_MIN - 1);
    TEST_ASSERT_TRUE(s.apShouldBeUp());
    s.tick(TEN_MIN); // 10 min sem cliente
    TEST_ASSERT_FALSE(s.apShouldBeUp());
}

static void test_clientPresence_renews()
{
    PortalSession s;
    s.requestOpen(0);
    s.noteClient(TEN_MIN - 1, true); // cliente conectado renova a atividade
    s.tick(TEN_MIN);
    TEST_ASSERT_TRUE(s.apShouldBeUp()); // não fechou: atividade recente
    s.tick((TEN_MIN - 1) + TEN_MIN);    // 10 min após a última atividade
    TEST_ASSERT_FALSE(s.apShouldBeUp());
}

static void test_reopenAfterClose()
{
    PortalSession s;
    s.requestOpen(0);
    s.tick(TEN_MIN);
    TEST_ASSERT_FALSE(s.apShouldBeUp());
    s.requestOpen(TEN_MIN + 5000);
    TEST_ASSERT_TRUE(s.apShouldBeUp());
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_startsClosed);
    RUN_TEST(test_requestOpen_raisesAp);
    RUN_TEST(test_idleTimeout_closes);
    RUN_TEST(test_clientPresence_renews);
    RUN_TEST(test_reopenAfterClose);
    UNITY_END();
}

void loop() {}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_session`
Expected: FALHA de compilação — `PortalSession.h` não existe.

- [ ] **Step 3: Implementar o header**

`src/modules/irrigation/PortalSession.h`:

```cpp
#pragma once
#include <stdint.h>

// Ciclo de vida do AP do captive portal (spec §7.2, §8.7). Timer puro, sem Arduino/WiFi:
// requestOpen() abre por PORTAL_TIMEOUT_MS; cada tick com cliente presente renova a atividade;
// sem cliente por PORTAL_TIMEOUT_MS o AP fecha. A cola (PortalAp) consome apShouldBeUp().
class PortalSession
{
  public:
    static constexpr uint32_t PORTAL_TIMEOUT_MS = 10u * 60u * 1000u; // 10 min (spec §7.2)

    enum class State : uint8_t { CLOSED = 0, OPEN = 1 };

    void requestOpen(uint32_t nowMs);          // botão / auto-open de fábrica
    void noteClient(uint32_t nowMs, bool anyClient); // renova a atividade enquanto houver cliente
    void tick(uint32_t nowMs);                 // fecha por inatividade
    bool apShouldBeUp() const { return _state == State::OPEN; }
    uint32_t secondsLeft(uint32_t nowMs) const;

  private:
    State _state = State::CLOSED;
    uint32_t _lastActivityMs = 0;
};
```

- [ ] **Step 4: Implementar o .cpp**

`src/modules/irrigation/PortalSession.cpp`:

```cpp
#include "modules/irrigation/PortalSession.h"

void PortalSession::requestOpen(uint32_t nowMs)
{
    _state = State::OPEN;
    _lastActivityMs = nowMs;
}

void PortalSession::noteClient(uint32_t nowMs, bool anyClient)
{
    if (_state == State::OPEN && anyClient)
        _lastActivityMs = nowMs;
}

void PortalSession::tick(uint32_t nowMs)
{
    if (_state != State::OPEN)
        return;
    if (nowMs - _lastActivityMs >= PORTAL_TIMEOUT_MS)
        _state = State::CLOSED;
}

uint32_t PortalSession::secondsLeft(uint32_t nowMs) const
{
    if (_state != State::OPEN)
        return 0;
    uint32_t elapsed = nowMs - _lastActivityMs;
    if (elapsed >= PORTAL_TIMEOUT_MS)
        return 0;
    return (PORTAL_TIMEOUT_MS - elapsed) / 1000;
}
```

- [ ] **Step 5: Bumpar a contagem de suítes**

Editar `test/native-suite-count`: trocar `44` por `45`.

- [ ] **Step 6: Rodar e ver passar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_session`
Expected: GREEN (5 testes OK).

- [ ] **Step 7: Commit**

```bash
git add src/modules/irrigation/PortalSession.h src/modules/irrigation/PortalSession.cpp test/test_portal_session/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): portal AP lifecycle state machine"
```

---

## Task 2: `PortalApi` — estado do nó ("Este nó") + parse do teste de pulso

Camada pura de build/parse JSON. Reusa `IrrigationWeb::JsonWriter`/`JsonReader`/`ParseResult` (não reimplementar).

**Files:**
- Create: `src/modules/irrigation/PortalApi.h`
- Create: `src/modules/irrigation/PortalApi.cpp`
- Create: `test/test_portal_api/test_main.cpp`
- Modify: `test/native-suite-count`

- [ ] **Step 1: Escrever o teste que falha**

`test/test_portal_api/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/PortalApi.h"
#include <string.h>
#include <unity.h>

using namespace IrrigationWeb;

void setUp(void) {}
void tearDown(void) {}

static bool contains(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }

static void test_buildNodeState_json()
{
    NodeStateCtx c = {};
    c.role = 0; // ESTACAO
    c.name = "Pasto Norte";
    c.boundGateway = 0x1234;
    c.configEpoch = 7;
    c.safeMode = false;
    c.numValves = 2;
    c.valveStates = 0x01;
    c.gpoStates = 0;
    c.vbatCentiV = 1250;
    c.flags = 0;
    c.apSecondsLeft = 540;
    char buf[512];
    size_t n = buildNodeState(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"role\":0"));
    TEST_ASSERT_TRUE(contains(buf, "\"name\":\"Pasto Norte\""));
    TEST_ASSERT_TRUE(contains(buf, "\"boundGateway\":4660")); // 0x1234
    TEST_ASSERT_TRUE(contains(buf, "\"valveStates\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"vbatCentiV\":1250"));
    TEST_ASSERT_TRUE(contains(buf, "\"apSecondsLeft\":540"));
}

static void test_buildNodeState_truncationReturnsZero()
{
    NodeStateCtx c = {};
    c.name = "x";
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(0, buildNodeState(c, buf, sizeof(buf)));
}

static void test_parsePulse_valid()
{
    PortalPulseReq p = {};
    ParseResult r = parsePulse("{\"valveId\":1,\"durationS\":10}", 28, p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(1, p.valveId);
    TEST_ASSERT_EQUAL_UINT16(10, p.durationS);
}

static void test_parsePulse_rejectsBadValve()
{
    PortalPulseReq p = {};
    ParseResult r = parsePulse("{\"valveId\":9,\"durationS\":10}", 28, p);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_parsePulse_rejectsBadDuration()
{
    PortalPulseReq p = {};
    ParseResult r = parsePulse("{\"valveId\":0,\"durationS\":0}", 27, p);
    TEST_ASSERT_FALSE(r.ok);
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_buildNodeState_json);
    RUN_TEST(test_buildNodeState_truncationReturnsZero);
    RUN_TEST(test_parsePulse_valid);
    RUN_TEST(test_parsePulse_rejectsBadValve);
    RUN_TEST(test_parsePulse_rejectsBadDuration);
    UNITY_END();
}

void loop() {}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_api`
Expected: FALHA — `PortalApi.h` não existe.

- [ ] **Step 3: Implementar o header**

`src/modules/irrigation/PortalApi.h`:

```cpp
#pragma once
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/IrrigationWebApi.h" // JsonWriter/JsonReader/ParseResult (reuso)
#include <cstddef>
#include <cstdint>

namespace IrrigationWeb
{

// --- Aba "Este nó" ---
struct NodeStateCtx {
    uint8_t role = 0;          // IrrigationRole
    const char *name = "";
    uint32_t boundGateway = 0; // 0 = não pareado
    uint32_t configEpoch = 0;
    bool safeMode = false;
    uint8_t numValves = 0;
    uint8_t valveStates = 0;   // bitmap
    uint8_t gpoStates = 0;     // bitmap
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0; // 0 se não medido
    uint8_t flags = 0;         // HbFlags (tamper/safe/hibernation)
    uint32_t apSecondsLeft = 0;
};
size_t buildNodeState(const NodeStateCtx &ctx, char *buf, size_t cap);

struct PortalPulseReq {
    uint8_t valveId = 0;
    uint16_t durationS = 0;
};
ParseResult parsePulse(const char *json, size_t len, PortalPulseReq &out);

} // namespace IrrigationWeb
```

- [ ] **Step 4: Implementar o .cpp**

`src/modules/irrigation/PortalApi.cpp`:

```cpp
#include "modules/irrigation/PortalApi.h"

namespace IrrigationWeb
{

size_t buildNodeState(const NodeStateCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyNum("role", ctx.role);
    w.keyStr("name", ctx.name);
    w.keyNum("boundGateway", (int64_t)ctx.boundGateway);
    w.keyNum("configEpoch", (int64_t)ctx.configEpoch);
    w.keyBool("safeMode", ctx.safeMode);
    w.keyNum("numValves", ctx.numValves);
    w.keyNum("valveStates", ctx.valveStates);
    w.keyNum("gpoStates", ctx.gpoStates);
    w.keyNum("vbatCentiV", ctx.vbatCentiV);
    w.keyNum("vpanelCentiV", ctx.vpanelCentiV);
    w.keyNum("flags", ctx.flags);
    w.keyNum("apSecondsLeft", (int64_t)ctx.apSecondsLeft);
    w.endObject();
    return w.done();
}

ParseResult parsePulse(const char *json, size_t len, PortalPulseReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t valveId = 0, dur = 0;
    if (!rd.getInt("valveId", valveId) || valveId < 0 || valveId > 7) r.fail("valveId fora de 0..7");
    if (!rd.getInt("durationS", dur) || dur < 1 || dur > 7200) r.fail("durationS fora de 1..7200");
    if (!r.ok) return r;
    out.valveId = (uint8_t)valveId;
    out.durationS = (uint16_t)dur;
    return r;
}

} // namespace IrrigationWeb
```

- [ ] **Step 5: Bumpar a contagem de suítes**

Editar `test/native-suite-count`: trocar `45` por `46`.

- [ ] **Step 6: Rodar e ver passar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_api`
Expected: GREEN (5 testes OK).

- [ ] **Step 7: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): portal node-state serializer + pulse parser"
```

---

## Task 3: Cola do módulo — estado do nó, teste de pulso, sessão do portal

Métodos de serviço no `IrrigationModule` chamados pela cola HTTP (role-agnósticos). Módulo não é testado no host; validação por build de CI (a corrida filtrada compila o firmware inteiro).

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`
- Modify: `src/modules/irrigation/IrrigationModule.cpp`

- [ ] **Step 1: Declarar forward-decls e métodos no header**

Em `src/modules/irrigation/IrrigationModule.h`, expandir o bloco de forward-decl (linha ~17) e incluir o header da sessão no topo:

```cpp
#include "modules/irrigation/PortalSession.h"
```

```cpp
// Forward-decl da cola web/portal (definidas em IrrigationWebApi.h/PortalApi.h, incluídas só no .cpp).
namespace IrrigationWeb
{
struct WebCommand;
struct NodeStateCtx;
struct PortalPulseReq;
}
```

Na seção pública, após `gwRunCommand(...)`, adicionar:

```cpp
    // --- Serviço do portal de campo (todos os papéis). Chamados pela cola HTTP (IrrigationPortalEndpoints). ---
    void portalFillNodeState(IrrigationWeb::NodeStateCtx &out) const;
    bool portalPulse(const IrrigationWeb::PortalPulseReq &p);
    PortalSession &portalSession() { return portal; }
```

Na seção privada de membros, após `LedPatternController led;`, adicionar:

```cpp
    PortalSession portal; // ciclo de vida do AP do captive portal (Fase 5b)
```

- [ ] **Step 2: Implementar os métodos no .cpp**

Em `src/modules/irrigation/IrrigationModule.cpp`, incluir o header do PortalApi junto dos outros includes de irrigação no topo:

```cpp
#include "modules/irrigation/PortalApi.h"
```

Adicionar as implementações (junto do bloco de serviço do gateway, após `gwRunCommand`):

```cpp
// --- Serviço do portal de campo (Fase 5b). Role-agnóstico. ---
void IrrigationModule::portalFillNodeState(IrrigationWeb::NodeStateCtx &out) const
{
    out.role = settings.role;
    out.name = owner.short_name; // extern meshtastic_User& (mesmo uso de handlePairAnnounce)
    out.boundGateway = settings.boundGateway;
    out.configEpoch = settings.configEpoch;
    out.safeMode = safeMode;
    out.numValves = settings.numValves;
    out.valveStates = valves.stateBitmap();
    out.gpoStates = 0;
    out.vbatCentiV = batteryCentiV();
    out.vpanelCentiV = 0;
    out.flags = safeMode ? HB_FLAG_SAFE_MODE : 0;
    out.apSecondsLeft = portal.secondsLeft(millis());
}

bool IrrigationModule::portalPulse(const IrrigationWeb::PortalPulseReq &p)
{
    // Teste de pulso local: abre a válvula com fechamento automático pelo timer fail-safe.
    if (valves.open(p.valveId, p.durationS, settings.maxOpenConfigS, millis()) != ValveController::Result::OK)
        return false;
    sendEvento(EV_TEST_PULSE);
    return true;
}
```

- [ ] **Step 3: Validar por build de CI**

Este edit compila apenas no firmware ESP32/nativo completo. Rodar qualquer suíte para forçar o build do firmware inteiro e pegar erros de compilação/link:

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_api`
Expected: GREEN (o firmware compila com os novos métodos; suíte continua passando).

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): module portal node-state + local pulse service"
```

---

## Task 4: `PortalAp` — cola só-ESP32 do AP (softAP + DNS cativo + mDNS)

Sobe/derruba o AP conforme `PortalSession::apShouldBeUp()`. Só-ESP32, CI-gated. Excluída do build nativo.

**Files:**
- Create: `src/modules/irrigation/PortalAp.h`
- Create: `src/modules/irrigation/PortalAp.cpp`
- Modify: `variants/native/portduino.ini`

- [ ] **Step 1: Header**

`src/modules/irrigation/PortalAp.h`:

```cpp
#pragma once
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include <stdint.h>

// Cola só-ESP32 do captive portal (Fase 5b). Dirigida pela PortalSession do módulo:
// quando apShouldBeUp() vira true, sobe softAP (WPA2) + DNSServer cativo + mDNS; quando
// vira false, derruba tudo. Chamar portalApLoop() periodicamente (do runOnce do módulo).
void portalApLoop(uint32_t nowMs);

#endif
```

- [ ] **Step 2: Implementação**

`src/modules/irrigation/PortalAp.cpp`:

```cpp
#include "modules/irrigation/PortalAp.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "configuration.h"
#include "main.h" // owner
#include "modules/irrigation/IrrigationModule.h"
#include <DNSServer.h>
#include <WiFi.h>

static bool sApUp = false;
static DNSServer sDns;

// PIN de aplicação do portal (spec §7). Placeholder compilado; refinamento (PIN configurável
// via config) fica em follow-up. WPA2 exige >= 8 chars.
#ifndef IRRIGATION_PORTAL_PIN
#define IRRIGATION_PORTAL_PIN "irrig1234"
#endif

static void bringUp()
{
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "Irrigacao-%s", owner.short_name);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, IRRIGATION_PORTAL_PIN);
    sDns.start(53, "*", WiFi.softAPIP()); // DNS cativo: resolve tudo para o device
    LOG_INFO("Irrigation portal: AP up (%s)", ssid);
    sApUp = true;
}

static void tearDown()
{
    sDns.stop();
    WiFi.softAPdisconnect(true);
    LOG_INFO("Irrigation portal: AP down");
    sApUp = false;
}

void portalApLoop(uint32_t nowMs)
{
    if (!irrigationModule)
        return;
    PortalSession &s = irrigationModule->portalSession();
    // Renova atividade enquanto houver estação Wi-Fi associada ao AP.
    s.noteClient(nowMs, sApUp && WiFi.softAPgetStationNum() > 0);
    s.tick(nowMs);

    bool want = s.apShouldBeUp();
    if (want && !sApUp)
        bringUp();
    else if (!want && sApUp)
        tearDown();
    if (sApUp)
        sDns.processNextRequest();
}

#endif
```

- [ ] **Step 3: Excluir do build nativo**

Em `variants/native/portduino.ini`, após a linha de `IrrigationWebEndpoints.cpp` (linha 16), acrescentar:

```ini
  -<modules/irrigation/PortalAp.cpp>  ; cola AP só-ESP32 (WiFi/DNSServer); nativo exclui como mesh/wifi
```

- [ ] **Step 4: Chamar `portalApLoop` no runOnce do módulo**

Em `src/modules/irrigation/IrrigationModule.cpp`, no início de `runOnce()`, sob guarda de webserver, invocar o loop do AP. Localizar `int32_t IrrigationModule::runOnce()` e inserir logo após a abertura da função:

```cpp
#if !MESHTASTIC_EXCLUDE_WEBSERVER
    portalApLoop(millis());
#endif
```

E incluir o header no topo do .cpp (após os outros includes de irrigação):

```cpp
#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include "modules/irrigation/PortalAp.h"
#endif
```

- [ ] **Step 5: Validar por build de CI**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_session`
Expected: GREEN (nativo exclui `PortalAp.cpp`; firmware compila; suíte passa). A compilação ESP32 real é validada no job de CI de hardware.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalAp.h src/modules/irrigation/PortalAp.cpp variants/native/portduino.ini src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): portal Wi-Fi AP + captive DNS glue (ESP32)"
```

---

## Task 5: `IrrigationPortalEndpoints` — rotas `/api/portal/*` (Este nó)

Cola HTTP só-ESP32, mesmo idioma de `IrrigationWebEndpoints`. Registra `GET /api/portal/node` e `POST /api/portal/node/pulse`. Role-agnóstica (não exige gateway).

**Files:**
- Create: `src/modules/irrigation/IrrigationPortalEndpoints.h`
- Create: `src/modules/irrigation/IrrigationPortalEndpoints.cpp`
- Modify: `variants/native/portduino.ini`
- Modify: `src/mesh/http/ContentHandler.cpp`

- [ ] **Step 1: Header**

`src/modules/irrigation/IrrigationPortalEndpoints.h`:

```cpp
#pragma once
#if !MESHTASTIC_EXCLUDE_WEBSERVER

namespace httpsserver
{
class HTTPServer;
}

// Registra as rotas /api/portal/* no HTTPServer do Meshtastic (Fase 5b). Chamada por ContentHandler.
void registerIrrigationPortalHandlers(httpsserver::HTTPServer *server);

#endif
```

- [ ] **Step 2: Implementação**

`src/modules/irrigation/IrrigationPortalEndpoints.cpp`:

```cpp
#include "modules/irrigation/IrrigationPortalEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/PortalApi.h"

#include <Arduino.h>

#undef str
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <ResourceNode.hpp>

using namespace httpsserver;
using namespace IrrigationWeb;

static void sendJson(HTTPResponse *res, const char *body, int status = 200)
{
    res->setStatusCode(status);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->print(body);
}

static void sendParseErrors(HTTPResponse *res, const ParseResult &pr)
{
    char err[256];
    JsonWriter w(err, sizeof(err));
    w.beginObject();
    w.key("errors");
    w.beginArray();
    for (uint8_t i = 0; i < pr.errorCount; i++)
        w.str(pr.errors[i].msg);
    w.endArray();
    w.endObject();
    if (w.done() == 0) {
        sendJson(res, "{\"errors\":[\"erro\"]}", 400);
        return;
    }
    sendJson(res, err, 400);
}

static size_t readBody(HTTPRequest *req, char *buf, size_t cap)
{
    if (cap == 0)
        return 0;
    size_t n = req->readBytes(reinterpret_cast<byte *>(buf), cap - 1);
    if (n >= cap)
        n = cap - 1;
    buf[n] = '\0';
    return n;
}

static void hNode(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    NodeStateCtx c = {};
    irrigationModule->portalFillNodeState(c);
    char buf[512];
    if (!buildNodeState(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hNodePulse(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    PortalPulseReq p;
    ParseResult pr = parsePulse(body, nb, p);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalPulse(p)) {
        sendJson(res, "{\"errors\":[\"pulso rejeitado\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

void registerIrrigationPortalHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/portal/node", "GET", &hNode));
    server->registerNode(new ResourceNode("/api/portal/node/pulse", "POST", &hNodePulse));
}

#endif
```

- [ ] **Step 3: Excluir do build nativo**

Em `variants/native/portduino.ini`, após a linha de `PortalAp.cpp`, acrescentar:

```ini
  -<modules/irrigation/IrrigationPortalEndpoints.cpp>  ; cola HTTP só-ESP32 (esp32_https_server); nativo exclui
```

- [ ] **Step 4: Registrar no ContentHandler**

Em `src/mesh/http/ContentHandler.cpp`, incluir o header junto do de irrigação e chamar o registro logo após `registerIrrigationHandlers` nos DOIS pontos (secureServer ~linha 120 e insecureServer ~linha 144).

Include (perto do `#include` de `IrrigationWebEndpoints.h`, se houver; senão junto dos includes de módulos):

```cpp
#include "modules/irrigation/IrrigationPortalEndpoints.h"
```

Após cada `registerIrrigationHandlers(secureServer);` / `registerIrrigationHandlers(insecureServer);`:

```cpp
    registerIrrigationPortalHandlers(secureServer);
```
```cpp
    registerIrrigationPortalHandlers(insecureServer);
```

- [ ] **Step 5: Validar por build de CI**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_api`
Expected: GREEN (nativo exclui a cola; firmware compila).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.h src/modules/irrigation/IrrigationPortalEndpoints.cpp variants/native/portduino.ini src/mesh/http/ContentHandler.cpp
git commit -m "feat(irrigation): portal /api/portal/node endpoints (ESP32)"
```

---

## Task 6: Hook do botão — SHORT levanta o portal

Substituir o stub da Fase 5 (`IrrigationModule.cpp:732`) pela abertura da sessão do portal.

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.cpp:731-733`

- [ ] **Step 1: Trocar o stub**

Em `onButtonEvent`, no ramo da estação pareada, trocar:

```cpp
    case Ev::SHORT:
        LOG_INFO("Irrigation: portal request (Phase 5 stub)");
        break;
```

por:

```cpp
    case Ev::SHORT:
        portal.requestOpen(millis()); // Fase 5b: sobe o captive portal (10 min, spec §8.7)
        LOG_INFO("Irrigation: captive portal requested");
        break;
```

- [ ] **Step 2: Validar por build de CI**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_irrigation_ui`
Expected: GREEN.

- [ ] **Step 3: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): button short-press raises captive portal"
```

---

## Task 7: Frontend — aba "Este nó"

Página estática servida do LittleFS. Reusa `data/irrigacao/style.css` da Fase 5a.

**Files:**
- Create: `data/irrigacao/portal/index.html`
- Create: `data/irrigacao/portal/app.js`

- [ ] **Step 1: HTML**

`data/irrigacao/portal/index.html`:

```html
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Portal de Irrigação</title>
  <link rel="stylesheet" href="/irrigacao/style.css" />
</head>
<body>
  <header><h1>Portal do Nó</h1><span id="apLeft" class="muted"></span></header>
  <nav class="tabs">
    <button data-tab="node" class="active">Este nó</button>
    <button data-tab="net">Rede</button>
  </nav>

  <section id="tab-node" class="tab">
    <div id="nodeState" class="card">Carregando…</div>
    <form id="pulseForm" class="card">
      <h2>Teste de pulso</h2>
      <label>Válvula <input type="number" id="pulseValve" min="0" max="7" value="0" /></label>
      <label>Duração (s) <input type="number" id="pulseDur" min="1" max="7200" value="10" /></label>
      <button type="submit">Abrir</button>
      <span id="pulseMsg" class="muted"></span>
    </form>
  </section>

  <section id="tab-net" class="tab hidden">
    <div class="card muted">Aba Rede — Marco 2.</div>
  </section>

  <script src="/irrigacao/portal/app.js"></script>
</body>
</html>
```

- [ ] **Step 2: JS**

`data/irrigacao/portal/app.js`:

```javascript
const ROLES = ["Estação", "Gateway", "Repetidor", "Serviço"];

async function j(url, opts) {
  const r = await fetch(url, opts);
  const t = await r.text();
  let body = {};
  try { body = t ? JSON.parse(t) : {}; } catch (e) {}
  return { ok: r.ok, body };
}

function renderNode(s) {
  const el = document.getElementById("nodeState");
  const valves = [];
  for (let i = 0; i < s.numValves; i++) valves.push((s.valveStates >> i) & 1 ? "▉" : "▁");
  el.innerHTML = `
    <h2>${s.name || "(sem nome)"} <small class="muted">${ROLES[s.role] || s.role}</small></h2>
    <p>Bateria: <b>${(s.vbatCentiV / 100).toFixed(2)} V</b></p>
    <p>Válvulas: <span class="mono">${valves.join(" ") || "—"}</span></p>
    <p>Gateway vinculado: ${s.boundGateway ? "0x" + s.boundGateway.toString(16) : "não pareado"}</p>
    <p>Epoch: ${s.configEpoch} ${s.safeMode ? "· <b>modo seguro</b>" : ""}</p>`;
  document.getElementById("apLeft").textContent =
    s.apSecondsLeft ? `AP: ${Math.floor(s.apSecondsLeft / 60)}m${s.apSecondsLeft % 60}s` : "";
}

async function refresh() {
  const { ok, body } = await j("/api/portal/node");
  if (ok) renderNode(body);
}

document.querySelectorAll("nav.tabs button").forEach((b) =>
  b.addEventListener("click", () => {
    document.querySelectorAll("nav.tabs button").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    document.querySelectorAll(".tab").forEach((t) => t.classList.add("hidden"));
    document.getElementById("tab-" + b.dataset.tab).classList.remove("hidden");
  })
);

document.getElementById("pulseForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const valveId = +document.getElementById("pulseValve").value;
  const durationS = +document.getElementById("pulseDur").value;
  const { ok, body } = await j("/api/portal/node/pulse", {
    method: "POST",
    body: JSON.stringify({ valveId, durationS }),
  });
  document.getElementById("pulseMsg").textContent = ok ? "OK" : (body.errors || ["erro"]).join("; ");
  refresh();
});

refresh();
setInterval(refresh, 3000);
```

- [ ] **Step 3: Validar sintaxe do JS**

Run: `& "C:\Users\jmelo\Nodejs\node-v22.14.0-win-x64\node-v22.14.0-win-x64\node.exe" --check data/irrigacao/portal/app.js`
Expected: sem saída (sintaxe OK).

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/portal/index.html data/irrigacao/portal/app.js
git commit -m "feat(irrigation): portal frontend shell + Este nó tab"
```

**Marco 1 completo:** portal de campo funcional (AP + Este nó + teste de pulso) em qualquer nó.

---

# MARCO 2 — aba "Rede" (comando remoto via gateway)

## Task 8: Protocolo — `MSG_REMOTE_CMD` encode/decode

Mensagem nova: nó de campo → gateway. Corpo `{zoneId, action, durationS}`. Gateway mapeia zona→estação/válvula e re-emite pelo caminho autorizado.

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h`
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp`
- Modify: `test/test_irrigation_protocol/test_main.cpp`

- [ ] **Step 1: Escrever o teste que falha**

Adicionar em `test/test_irrigation_protocol/test_main.cpp` (uma função de teste nova + `RUN_TEST` no `setup()`):

```cpp
static void test_remoteCmd_roundtrip()
{
    using namespace IrrigationProto;
    uint8_t buf[32];
    RemoteCmd m = {};
    m.zoneId = 7;
    m.action = 1;
    m.durationS = 600;
    size_t n = encodeRemoteCmd(buf, sizeof(buf), 0x11223344, m);
    TEST_ASSERT_GREATER_THAN(0, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_REMOTE_CMD, h.type);
    TEST_ASSERT_EQUAL_UINT32(0x11223344, h.seq);

    RemoteCmd out = {};
    TEST_ASSERT_TRUE(decodeRemoteCmd(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(7, out.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, out.action);
    TEST_ASSERT_EQUAL_UINT16(600, out.durationS);
}

static void test_remoteCmd_shortBufferFails()
{
    using namespace IrrigationProto;
    uint8_t buf[4]; // menor que o header
    RemoteCmd m = {};
    TEST_ASSERT_EQUAL_UINT(0, encodeRemoteCmd(buf, sizeof(buf), 1, m));
}
```

Adicionar no `setup()`:

```cpp
    RUN_TEST(test_remoteCmd_roundtrip);
    RUN_TEST(test_remoteCmd_shortBufferFails);
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_irrigation_protocol`
Expected: FALHA — `MSG_REMOTE_CMD`/`RemoteCmd`/`encodeRemoteCmd` não existem.

- [ ] **Step 3: Declarar no header**

Em `src/modules/irrigation/IrrigationProtocol.h`, no enum `MsgType`, após `MSG_RESYNC_SEQ = 11,`:

```cpp
    MSG_REMOTE_CMD = 12,
```

Após `struct CmdGpo { ... };`:

```cpp
struct RemoteCmd {
    uint8_t zoneId;    // id de zona no gateway (1..255)
    uint8_t action;    // 0 = fechar, 1 = abrir
    uint16_t durationS;
};
```

Junto dos outros protótipos de encode/decode:

```cpp
size_t encodeRemoteCmd(uint8_t *buf, size_t len, uint32_t seq, const RemoteCmd &m);
bool decodeRemoteCmd(const uint8_t *buf, size_t len, RemoteCmd &out);
```

- [ ] **Step 4: Implementar no .cpp**

Em `src/modules/irrigation/IrrigationProtocol.cpp`, após `decodeCmdGpo`:

```cpp
size_t encodeRemoteCmd(uint8_t *buf, size_t len, uint32_t seq, const RemoteCmd &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_REMOTE_CMD, seq);
    w.u8(m.zoneId);
    w.u8(m.action);
    w.u16(m.durationS);
    return w.ok ? w.pos : 0;
}

bool decodeRemoteCmd(const uint8_t *buf, size_t len, RemoteCmd &out)
{
    Reader r = bodyReader(buf, len);
    out.zoneId = r.u8();
    out.action = r.u8();
    out.durationS = r.u16();
    return r.ok;
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_irrigation_protocol`
Expected: GREEN.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationProtocol.cpp test/test_irrigation_protocol/test_main.cpp
git commit -m "feat(irrigation): MSG_REMOTE_CMD protocol codec"
```

---

## Task 9: `PortalApi` — parse do comando de rede + roster

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h`
- Modify: `src/modules/irrigation/PortalApi.cpp`
- Modify: `test/test_portal_api/test_main.cpp`

- [ ] **Step 1: Escrever o teste que falha**

Adicionar em `test/test_portal_api/test_main.cpp`:

```cpp
static void test_parseNetCommand_open()
{
    NetCommand c = {};
    ParseResult r = parseNetCommand("{\"kind\":\"open\",\"zoneId\":3,\"durationS\":300}", 41, c);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(3, c.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, c.action);
    TEST_ASSERT_EQUAL_UINT16(300, c.durationS);
}

static void test_parseNetCommand_close()
{
    NetCommand c = {};
    ParseResult r = parseNetCommand("{\"kind\":\"close\",\"zoneId\":3}", 27, c);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(0, c.action);
}

static void test_parseNetCommand_rejectsBadZone()
{
    NetCommand c = {};
    ParseResult r = parseNetCommand("{\"kind\":\"open\",\"zoneId\":0,\"durationS\":10}", 40, c);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_buildRoster_gateway()
{
    ZoneTable zt;
    Zone z = {};
    z.id = 3;
    snprintf(z.name, sizeof(z.name), "Horta");
    z.node = 0xAA;
    zt.upsert(z);
    char buf[512];
    size_t n = buildRoster(&zt, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":3"));
    TEST_ASSERT_TRUE(contains(buf, "\"name\":\"Horta\""));
}

static void test_buildRoster_stationEmpty()
{
    char buf[64];
    size_t n = buildRoster(nullptr, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("[]", buf);
}
```

Adicionar no `setup()`:

```cpp
    RUN_TEST(test_parseNetCommand_open);
    RUN_TEST(test_parseNetCommand_close);
    RUN_TEST(test_parseNetCommand_rejectsBadZone);
    RUN_TEST(test_buildRoster_gateway);
    RUN_TEST(test_buildRoster_stationEmpty);
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_api`
Expected: FALHA — `NetCommand`/`parseNetCommand`/`buildRoster` não existem.

- [ ] **Step 3: Declarar no header**

Em `src/modules/irrigation/PortalApi.h`, antes do `} // namespace IrrigationWeb`:

```cpp
// --- Aba "Rede" ---
struct NetCommand {
    uint8_t zoneId = 0;
    uint8_t action = 0; // 0 = fechar, 1 = abrir
    uint16_t durationS = 0;
};
ParseResult parseNetCommand(const char *json, size_t len, NetCommand &out);

// Lista de alvos. zones != nullptr (gateway) => emite as zonas; nullptr (estação) => "[]".
size_t buildRoster(const ZoneTable *zones, char *buf, size_t cap);
```

- [ ] **Step 4: Implementar no .cpp**

Em `src/modules/irrigation/PortalApi.cpp`, antes do `} // namespace IrrigationWeb`, e incluir `<cstring>` no topo se necessário:

```cpp
ParseResult parseNetCommand(const char *json, size_t len, NetCommand &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    char kind[12] = {0};
    int64_t zoneId = 0, dur = 0;
    if (!rd.getStr("kind", kind, sizeof(kind))) { r.fail("kind ausente"); return r; }
    if (!rd.getInt("zoneId", zoneId) || zoneId < 1 || zoneId > 255) r.fail("zoneId fora de 1..255");
    if (strcmp(kind, "open") == 0) {
        if (!rd.getInt("durationS", dur) || dur < 1 || dur > 7200) r.fail("durationS fora de 1..7200");
        out.action = 1;
    } else if (strcmp(kind, "close") == 0) {
        out.action = 0;
        dur = 0;
    } else {
        r.fail("kind desconhecido");
    }
    if (!r.ok) return r;
    out.zoneId = (uint8_t)zoneId;
    out.durationS = (uint16_t)dur;
    return r;
}

size_t buildRoster(const ZoneTable *zones, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    if (zones) {
        for (size_t i = 0; i < zones->count(); i++) {
            const Zone *z = zones->zoneAt(i);
            if (!z) break;
            w.beginObject();
            w.keyNum("id", z->id);
            w.keyStr("name", z->name);
            w.keyNum("padraoMin", z->padraoMin);
            w.endObject();
        }
    }
    w.endArray();
    return w.done();
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_api`
Expected: GREEN.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_main.cpp
git commit -m "feat(irrigation): portal net-command parser + roster builder"
```

---

## Task 10: Cola do módulo — TX do comando remoto + handler no gateway

Estação: envia `MSG_REMOTE_CMD` ao gateway. Gateway: recebe, valida a zona, re-emite via `gwSendValveCmd`. Se o próprio nó for o gateway, aplica local (sem rádio).

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`
- Modify: `src/modules/irrigation/IrrigationModule.cpp`

- [ ] **Step 1: Declarar no header**

Em `src/modules/irrigation/IrrigationModule.h`, ampliar o forward-decl:

```cpp
namespace IrrigationWeb
{
struct WebCommand;
struct NodeStateCtx;
struct PortalPulseReq;
struct NetCommand;
}
```

Na seção pública, após `portalPulse(...)`:

```cpp
    bool portalRunNetCommand(const IrrigationWeb::NetCommand &c);
```

Na seção privada, junto dos outros `handle*`:

```cpp
    void handleRemoteCmd(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
```

- [ ] **Step 2: Implementar `portalRunNetCommand`**

Em `src/modules/irrigation/IrrigationModule.cpp`, após `portalPulse`:

```cpp
bool IrrigationModule::portalRunNetCommand(const IrrigationWeb::NetCommand &c)
{
    if (gwIsGateway()) {
        // Este nó é o gateway: aplica local sem rádio (reusa a validação de zona do gateway).
        const Zone *z = gateway.zones.byId(c.zoneId);
        if (!z)
            return false;
        const StationEntry *st = gateway.stations.byNode(z->node);
        uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
        if (c.action == 1) {
            uint16_t dur = c.durationS;
            if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60))
                dur = (uint16_t)(z->maxMin * 60);
            gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts);
        } else {
            gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
        }
        return true;
    }
    // Nó de campo: encaminha ao gateway vinculado por rádio.
    if (settings.boundGateway == 0)
        return false;
    IrrigationProto::RemoteCmd m = {};
    m.zoneId = c.zoneId;
    m.action = c.action;
    m.durationS = c.durationS;
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = settings.boundGateway;
    p->decoded.payload.size =
        (uint16_t)IrrigationProto::encodeRemoteCmd(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, m);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return false;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    return true;
}
```

- [ ] **Step 3: Implementar `handleRemoteCmd` (lado gateway)**

Em `src/modules/irrigation/IrrigationModule.cpp`, junto dos outros `handleGw*`:

```cpp
void IrrigationModule::handleRemoteCmd(const meshtastic_MeshPacket &mp, const Header &h)
{
    // Autoridade = posse da PSK da fazenda (spec §7.2); anti-replay por seq ainda vale.
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation GW: replayed remote-cmd seq %u from 0x%08x", h.seq, mp.from);
        return;
    }
    if (!rateLimiter.allow(millis())) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_RATE_LIMIT);
        return;
    }
    RemoteCmd cmd;
    if (!decodeRemoteCmd(mp.decoded.payload.bytes, mp.decoded.payload.size, cmd)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        return;
    }
    IrrigationWeb::NetCommand nc = {};
    nc.zoneId = cmd.zoneId;
    nc.action = cmd.action;
    nc.durationS = cmd.durationS;
    bool ok = portalRunNetCommand(nc); // gateway => aplica local
    sendAck(mp.from, h.seq, ok ? ACK_OK : ACK_NACK, ok ? REASON_NONE : REASON_INVALID_ID);
}
```

- [ ] **Step 4: Despachar em `handleReceived`**

Em `src/modules/irrigation/IrrigationModule.cpp`, no `switch (h.type)` de `handleReceived`, antes do `default:`:

```cpp
    case MSG_REMOTE_CMD:
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            handleRemoteCmd(mp, h);
        else
            LOG_DEBUG("Irrigation: REMOTE_CMD from 0x%08x ignored (role=%d)", mp.from, settings.role);
        break;
```

- [ ] **Step 5: Validar por build de CI**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_irrigation_protocol`
Expected: GREEN (firmware compila com o handler + dispatch).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): remote-command TX + gateway relay handler"
```

---

## Task 11: Endpoints `/api/portal/net/*`

**Files:**
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp`

- [ ] **Step 1: Adicionar handlers e rotas**

Em `src/modules/irrigation/IrrigationPortalEndpoints.cpp`, adicionar os handlers antes de `registerIrrigationPortalHandlers`:

```cpp
static void hNetRoster(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    const ZoneTable *zones = irrigationModule->gwIsGateway() ? &irrigationModule->gwState().zones : nullptr;
    char buf[2048];
    if (!buildRoster(zones, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hNetCommand(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    NetCommand c;
    ParseResult pr = parseNetCommand(body, nb, c);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalRunNetCommand(c)) {
        sendJson(res, "{\"errors\":[\"comando rejeitado (sem gateway ou zona inexistente)\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}
```

E registrar dentro de `registerIrrigationPortalHandlers`, após as rotas de node:

```cpp
    server->registerNode(new ResourceNode("/api/portal/net/roster", "GET", &hNetRoster));
    server->registerNode(new ResourceNode("/api/portal/net/command", "POST", &hNetCommand));
```

- [ ] **Step 2: Validar por build de CI**

Run: `docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_api`
Expected: GREEN (nativo exclui a cola; firmware compila).

- [ ] **Step 3: Commit**

```bash
git add src/modules/irrigation/IrrigationPortalEndpoints.cpp
git commit -m "feat(irrigation): portal /api/portal/net endpoints (ESP32)"
```

---

## Task 12: Frontend — aba "Rede"

**Files:**
- Modify: `data/irrigacao/portal/index.html`
- Modify: `data/irrigacao/portal/app.js`

- [ ] **Step 1: Substituir o placeholder da aba Rede no HTML**

Em `data/irrigacao/portal/index.html`, trocar:

```html
  <section id="tab-net" class="tab hidden">
    <div class="card muted">Aba Rede — Marco 2.</div>
  </section>
```

por:

```html
  <section id="tab-net" class="tab hidden">
    <form id="netForm" class="card">
      <h2>Comandar zona (via gateway)</h2>
      <label>Zona <select id="netZone"></select></label>
      <label>Ação
        <select id="netAction"><option value="open">Abrir</option><option value="close">Fechar</option></select>
      </label>
      <label>Duração (s) <input type="number" id="netDur" min="1" max="7200" value="300" /></label>
      <button type="submit">Enviar</button>
      <span id="netMsg" class="muted"></span>
    </form>
  </section>
```

- [ ] **Step 2: Adicionar a lógica da aba Rede no JS**

Em `data/irrigacao/portal/app.js`, antes de `refresh();` no final, adicionar:

```javascript
async function loadRoster() {
  const { ok, body } = await j("/api/portal/net/roster");
  const sel = document.getElementById("netZone");
  sel.innerHTML = "";
  if (ok && Array.isArray(body) && body.length) {
    body.forEach((z) => {
      const o = document.createElement("option");
      o.value = z.id;
      o.textContent = `${z.id} — ${z.name}`;
      sel.appendChild(o);
    });
  } else {
    // Estação sem roster: permite digitar o número da zona (1..255).
    for (let i = 1; i <= 24; i++) {
      const o = document.createElement("option");
      o.value = i;
      o.textContent = "Zona " + i;
      sel.appendChild(o);
    }
  }
}

document.getElementById("netForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const zoneId = +document.getElementById("netZone").value;
  const kind = document.getElementById("netAction").value;
  const durationS = +document.getElementById("netDur").value;
  const payload = kind === "open" ? { kind, zoneId, durationS } : { kind, zoneId };
  const { ok, body } = await j("/api/portal/net/command", {
    method: "POST",
    body: JSON.stringify(payload),
  });
  document.getElementById("netMsg").textContent = ok ? "Enviado" : (body.errors || ["erro"]).join("; ");
});

loadRoster();
```

- [ ] **Step 3: Validar sintaxe do JS**

Run: `& "C:\Users\jmelo\Nodejs\node-v22.14.0-win-x64\node-v22.14.0-win-x64\node.exe" --check data/irrigacao/portal/app.js`
Expected: sem saída (sintaxe OK).

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/portal/index.html data/irrigacao/portal/app.js
git commit -m "feat(irrigation): portal Rede tab (remote command via gateway)"
```

**Marco 2 completo:** operar a fazenda a partir de qualquer nó de campo.

---

## Verificação final

- [ ] Rodar a suíte completa das irrigações (spot-check das novas + regressão):

```powershell
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_session
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_portal_api
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f test_irrigation_protocol
```

Expected: três GREEN.

- [ ] Confirmar `test/native-suite-count` == `46`.
- [ ] Após push: verificar o job de CI ESP32 (tbeam/gateway) — `PortalAp.cpp` e `IrrigationPortalEndpoints.cpp` só compilam lá. `trunk fmt` roda em CI.

## Follow-ups conhecidos (deferidos)

- **Config local ("Este nó" write)**: coordenadas próprias do nó exigem um campo em `IrrigationSettings` (bump ABI v4) ou um arquivo LittleFS separado. Nesta fase a aba é read-only + pulso. Endpoint `/api/portal/node/config` fica para depois.
- **Roster com nomes na estação**: nós de campo mostram números de zona (o gateway valida). Sincronizar nomes/roster = fase futura.
- **PIN do portal configurável**: hoje `IRRIGATION_PORTAL_PIN` é compilado; expor na config = follow-up.
- **Conflito de modo Wi-Fi**: `PortalAp` assume `WIFI_AP`; coexistência com `WiFiAPClient` em modo estação precisa de ajuste após validação de hardware.
- **Aba Instalador (§8.4) + survey (§8.5)**: removidos do escopo — firmware específico.
