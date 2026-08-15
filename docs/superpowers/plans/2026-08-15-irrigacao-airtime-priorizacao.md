# Irrigação — Priorização de airtime e back-pressure — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fazer comando de válvula ter prioridade absoluta no airtime e reduzir uso crônico do canal via prioridade por tipo, gate de back-pressure e heartbeat adaptativo (backoff + jitter).

**Architecture:** Extrair as decisões puras (prioridade por tipo, tipo gated, fator de backoff, offset de jitter, próximo HB devido) para um módulo livre `IrrigationAirtime` totalmente testável. `IrrigationModule` passa a enviar todo pacote por um único `txPacket()` que aplica prioridade + gate. O agendamento do heartbeat consome as funções puras.

**Tech Stack:** C++ (Meshtastic firmware), Unity (test framework nativo/portduino), PlatformIO. API nativa `AirTime` (`src/airtime.h`).

## Global Constraints

- Comando de válvula NUNCA é barrado pelo gate (impolite). Copiado verbatim do spec: "comando de válvula tem prioridade absoluta".
- Backward-compat de ABI on-disk: sem bump de versão de blob, sem mudança de `sizeof(IrrigationSettings)`. Reuso de `pad3` (offset 207).
- Testes: `./bin/run-tests.sh` deve terminar GREEN (exit 0).
- Seguir padrão dos testes de irrigação existentes: unidades puras testadas diretamente (`test/test_irrigation_protocol`, `test/test_irrigation_monitor`), `void setUp()/tearDown()`, sem `int main()`.
- Formatação: `trunk fmt` (ou clang-format@16.0.3 nas linhas alteradas — ver `trunk-fmt-windows-workaround`).

## File Structure

- Create: `src/modules/irrigation/IrrigationAirtime.h` — declarações das funções puras de decisão.
- Create: `src/modules/irrigation/IrrigationAirtime.cpp` — implementações puras.
- Create: `test/test_irrigation_airtime/test_main.cpp` — suíte Unity das funções puras.
- Modify: `src/modules/irrigation/IrrigationModule.h` — declarar `bool txPacket(meshtastic_MeshPacket *p)`.
- Modify: `src/modules/irrigation/IrrigationModule.cpp` — implementar `txPacket`, trocar os ~25 `service->sendToMesh(p, RX_SRC_LOCAL, false)`, reescrever o agendamento do heartbeat (loop ~1064-1076), re-seed de jitter no epoch, override de telemetria nativa no setup.
- Modify: `src/modules/irrigation/IrrigationSettings.h` — renomear `pad3` → `hbTrafficFlags` + constantes de bit + acessores inline.

---

### Task 1: Funções puras de decisão de airtime

**Files:**
- Create: `src/modules/irrigation/IrrigationAirtime.h`
- Create: `src/modules/irrigation/IrrigationAirtime.cpp`
- Test: `test/test_irrigation_airtime/test_main.cpp`

**Interfaces:**
- Consumes: `IrrigationProto::MsgType`, `EventCode` de `IrrigationProtocol.h`.
- Produces (usadas nas Tasks 2 e 3):
  - `uint8_t IrrigationAirtime::priorityForType(uint8_t msgType, bool criticalEvent)` — retorna valor `meshtastic_MeshPacket_Priority`.
  - `bool IrrigationAirtime::isGatedType(uint8_t msgType)` — true p/ HEARTBEAT e PING_SURVEY.
  - `bool IrrigationAirtime::isCriticalEvent(uint8_t evCode)` — true p/ EV_TAMPER (demais eventos = não crítico).
  - `uint8_t IrrigationAirtime::hbBackoffFactor(float chUtilPercent)` — 1/2/4/8.
  - `uint32_t IrrigationAirtime::hbJitterOffsetMs(uint32_t nodeNum, uint32_t epoch, uint32_t windowMs)` — offset determinístico `< windowMs` (retorna 0 se windowMs==0).
  - `uint32_t IrrigationAirtime::hbWindowMs(uint32_t effectiveIntervalMs)` — `min(effectiveIntervalMs/4, 30000)`.

- [ ] **Step 1: Escrever o header**

Create `src/modules/irrigation/IrrigationAirtime.h`:

```cpp
#pragma once
#include <stdint.h>

// Decisões puras de gerenciamento de airtime do módulo de irrigação.
// Sem estado, sem dependência de AirTime real — testáveis em nativo.
namespace IrrigationAirtime
{

// Prioridade meshtastic_MeshPacket_Priority por tipo IrrigationProto::MsgType.
// criticalEvent só é consultado quando msgType == MSG_EVENTO.
uint8_t priorityForType(uint8_t msgType, bool criticalEvent);

// true → o pacote cede airtime sob congestionamento (HEARTBEAT, PING_SURVEY).
bool isGatedType(uint8_t msgType);

// Evento que escala para prioridade ALERT (tamper). Demais = rotina.
bool isCriticalEvent(uint8_t evCode);

// Multiplicador do intervalo de heartbeat conforme utilização do canal (%).
// <25→1, [25,40)→2, [40,60)→4, >=60→8.
uint8_t hbBackoffFactor(float chUtilPercent);

// Janela de jitter: min(effectiveIntervalMs/4, 30000).
uint32_t hbWindowMs(uint32_t effectiveIntervalMs);

// Offset determinístico de jitter em [0, windowMs). Muda com epoch. 0 se windowMs==0.
uint32_t hbJitterOffsetMs(uint32_t nodeNum, uint32_t epoch, uint32_t windowMs);

} // namespace IrrigationAirtime
```

- [ ] **Step 2: Escrever os testes que falham**

Create `test/test_irrigation_airtime/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationAirtime.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <unity.h>

using namespace IrrigationAirtime;
using namespace IrrigationProto;

void setUp(void) {}
void tearDown(void) {}

static void test_priority_command_is_high()
{
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_HIGH, priorityForType(MSG_CMD_VALVULA, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_HIGH, priorityForType(MSG_CMD_GPO, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_HIGH, priorityForType(MSG_REMOTE_CMD, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_HIGH, priorityForType(MSG_REMOTE_TRIGGER, false));
}

static void test_priority_ack_and_hb_and_survey()
{
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_RESPONSE, priorityForType(MSG_ACK, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_BACKGROUND, priorityForType(MSG_HEARTBEAT, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_BACKGROUND, priorityForType(MSG_PING_SURVEY, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_DEFAULT, priorityForType(MSG_SET_CONFIG, false));
}

static void test_priority_event_critical_vs_routine()
{
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_ALERT, priorityForType(MSG_EVENTO, true));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_DEFAULT, priorityForType(MSG_EVENTO, false));
    TEST_ASSERT_EQUAL_UINT8(meshtastic_MeshPacket_Priority_ALERT, priorityForType(MSG_CMD_MAINT, false));
}

static void test_isGatedType()
{
    TEST_ASSERT_TRUE(isGatedType(MSG_HEARTBEAT));
    TEST_ASSERT_TRUE(isGatedType(MSG_PING_SURVEY));
    TEST_ASSERT_FALSE(isGatedType(MSG_CMD_VALVULA));
    TEST_ASSERT_FALSE(isGatedType(MSG_ACK));
    TEST_ASSERT_FALSE(isGatedType(MSG_EVENTO));
}

static void test_isCriticalEvent()
{
    TEST_ASSERT_TRUE(isCriticalEvent(EV_TAMPER));
    TEST_ASSERT_FALSE(isCriticalEvent(EV_MANUAL_OPEN));
    TEST_ASSERT_FALSE(isCriticalEvent(EV_PAIRED));
}

static void test_hbBackoffFactor_bands()
{
    TEST_ASSERT_EQUAL_UINT8(1, hbBackoffFactor(0.0f));
    TEST_ASSERT_EQUAL_UINT8(1, hbBackoffFactor(24.9f));
    TEST_ASSERT_EQUAL_UINT8(2, hbBackoffFactor(25.0f));
    TEST_ASSERT_EQUAL_UINT8(2, hbBackoffFactor(39.9f));
    TEST_ASSERT_EQUAL_UINT8(4, hbBackoffFactor(40.0f));
    TEST_ASSERT_EQUAL_UINT8(4, hbBackoffFactor(59.9f));
    TEST_ASSERT_EQUAL_UINT8(8, hbBackoffFactor(60.0f));
    TEST_ASSERT_EQUAL_UINT8(8, hbBackoffFactor(100.0f));
}

static void test_hbWindowMs_caps_at_30s()
{
    TEST_ASSERT_EQUAL_UINT32(15000, hbWindowMs(60000));   // 60s/4
    TEST_ASSERT_EQUAL_UINT32(30000, hbWindowMs(600000));  // 10min/4 -> cap 30s
    TEST_ASSERT_EQUAL_UINT32(0, hbWindowMs(0));
}

static void test_hbJitterOffset_deterministic_in_window()
{
    uint32_t w = 30000;
    uint32_t a = hbJitterOffsetMs(0x11111111, 5, w);
    uint32_t b = hbJitterOffsetMs(0x11111111, 5, w);
    TEST_ASSERT_EQUAL_UINT32(a, b);        // determinístico
    TEST_ASSERT_LESS_THAN_UINT32(w, a);    // dentro da janela

    uint32_t c = hbJitterOffsetMs(0x11111111, 6, w);
    TEST_ASSERT_NOT_EQUAL(a, c);           // muda com epoch

    uint32_t d = hbJitterOffsetMs(0x22222222, 5, w);
    TEST_ASSERT_NOT_EQUAL(a, d);           // muda com nodeNum

    TEST_ASSERT_EQUAL_UINT32(0, hbJitterOffsetMs(0x11111111, 5, 0)); // janela 0
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_priority_command_is_high);
    RUN_TEST(test_priority_ack_and_hb_and_survey);
    RUN_TEST(test_priority_event_critical_vs_routine);
    RUN_TEST(test_isGatedType);
    RUN_TEST(test_isCriticalEvent);
    RUN_TEST(test_hbBackoffFactor_bands);
    RUN_TEST(test_hbWindowMs_caps_at_30s);
    RUN_TEST(test_hbJitterOffset_deterministic_in_window);
    UNITY_END();
}

void loop() {}
```

- [ ] **Step 3: Rodar o teste e confirmar que falha (link/compile error)**

Run: `./bin/run-tests.sh test_irrigation_airtime`
Expected: RED — `IrrigationAirtime.cpp` ainda não existe / símbolos indefinidos.

- [ ] **Step 4: Implementar as funções puras**

Create `src/modules/irrigation/IrrigationAirtime.cpp`:

```cpp
#include "IrrigationAirtime.h"
#include "IrrigationProtocol.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

namespace IrrigationAirtime
{

using namespace IrrigationProto;

uint8_t priorityForType(uint8_t msgType, bool criticalEvent)
{
    switch (msgType) {
    case MSG_CMD_VALVULA:
    case MSG_CMD_GPO:
    case MSG_REMOTE_CMD:
    case MSG_REMOTE_TRIGGER:
        return meshtastic_MeshPacket_Priority_HIGH;
    case MSG_CMD_MAINT:
        return meshtastic_MeshPacket_Priority_ALERT;
    case MSG_ACK:
        return meshtastic_MeshPacket_Priority_RESPONSE;
    case MSG_EVENTO:
        return criticalEvent ? meshtastic_MeshPacket_Priority_ALERT : meshtastic_MeshPacket_Priority_DEFAULT;
    case MSG_HEARTBEAT:
    case MSG_PING_SURVEY:
        return meshtastic_MeshPacket_Priority_BACKGROUND;
    default:
        return meshtastic_MeshPacket_Priority_DEFAULT;
    }
}

bool isGatedType(uint8_t msgType)
{
    return msgType == MSG_HEARTBEAT || msgType == MSG_PING_SURVEY;
}

bool isCriticalEvent(uint8_t evCode)
{
    return evCode == EV_TAMPER;
}

uint8_t hbBackoffFactor(float chUtilPercent)
{
    if (chUtilPercent >= 60.0f)
        return 8;
    if (chUtilPercent >= 40.0f)
        return 4;
    if (chUtilPercent >= 25.0f)
        return 2;
    return 1;
}

uint32_t hbWindowMs(uint32_t effectiveIntervalMs)
{
    uint32_t quarter = effectiveIntervalMs / 4;
    return quarter < 30000u ? quarter : 30000u;
}

uint32_t hbJitterOffsetMs(uint32_t nodeNum, uint32_t epoch, uint32_t windowMs)
{
    if (windowMs == 0)
        return 0;
    // FNV-1a 32-bit sobre (nodeNum ^ epoch): determinístico, boa dispersão.
    uint32_t x = nodeNum ^ epoch;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) {
        h ^= (x & 0xff);
        h *= 16777619u;
        x >>= 8;
    }
    return h % windowMs;
}

} // namespace IrrigationAirtime
```

- [ ] **Step 5: Rodar o teste e confirmar GREEN**

Run: `./bin/run-tests.sh test_irrigation_airtime`
Expected: PASS — 8 testes verdes.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationAirtime.h src/modules/irrigation/IrrigationAirtime.cpp test/test_irrigation_airtime/test_main.cpp
git commit -m "feat(irrigation): funcoes puras de decisao de airtime (prioridade/gate/backoff/jitter)"
```

---

### Task 2: Choke point `txPacket` + prioridade + gate

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarar método privado)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (implementar + trocar sites de envio)

**Interfaces:**
- Consumes: `IrrigationAirtime::priorityForType`, `isGatedType`, `isCriticalEvent` (Task 1); `airTime` global (`src/airtime.h`); `service->sendToMesh`.
- Produces: `bool IrrigationModule::txPacket(meshtastic_MeshPacket *p)` — usado pela Task 3 e por todos os emissores.

- [ ] **Step 1: Declarar o método**

Em `src/modules/irrigation/IrrigationModule.h`, na seção privada de métodos (perto de `sendHeartbeat`), adicionar:

```cpp
    // Choke point de envio: aplica prioridade por tipo e o gate de airtime
    // (HB/survey cedem sob congestionamento; comando/ACK/alarme sempre passam).
    // Devolve true se enviado, false se barrado (pacote é liberado).
    bool txPacket(meshtastic_MeshPacket *p);
```

- [ ] **Step 2: Implementar `txPacket`**

Em `src/modules/irrigation/IrrigationModule.cpp`, adicionar includes no topo (junto aos includes de irrigação existentes):

```cpp
#include "IrrigationAirtime.h"
#include "airtime.h"
```

E implementar (perto de `sendHeartbeat`):

```cpp
bool IrrigationModule::txPacket(meshtastic_MeshPacket *p)
{
    // Tipo IrrigationProto vive em payload.bytes[1] (Header.type). arg do evento em bytes[8..].
    uint8_t type = p->decoded.payload.size > 1 ? p->decoded.payload.bytes[1] : 0;
    bool crit = false;
    if (type == IrrigationProto::MSG_EVENTO && p->decoded.payload.size > IrrigationProto::HEADER_LEN)
        crit = IrrigationAirtime::isCriticalEvent(p->decoded.payload.bytes[IrrigationProto::HEADER_LEN]);

    p->priority = (meshtastic_MeshPacket_Priority)IrrigationAirtime::priorityForType(type, crit);

    if (IrrigationAirtime::isGatedType(type) && airTime && !airTime->isTxAllowedChannelUtil(/*polite=*/true)) {
        LOG_DEBUG("Irrigation tx gated (type=%u, chUtil high)", type);
        packetPool.release(p);
        return false;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    return true;
}
```

- [ ] **Step 3: Trocar todos os sites de envio**

Substituir cada ocorrência de `service->sendToMesh(p, RX_SRC_LOCAL, false);` em `IrrigationModule.cpp` por `txPacket(p);`. São ~25 sites (linhas aproximadas 605, 637, 698, 723, 932, 994, 1100, 1139, 1212, 1389, 1415, 1456, 1534, 1609, 1631, 1646, 1801, 2599, 2619, 3354, 3709, 3864, 4392). Verificar cada um: onde havia `service->sendToMesh(p, ...)` logo após um `encode*`/`allocDataPacket`, trocar pela chamada a `txPacket(p)`.

NÃO trocar chamadas `service->sendToMesh` que enviem pacotes fora do protocolo de irrigação (ex.: pacotes de canal/admin sem header IrrigationProto), se houver. Regra: só trocar onde o payload foi produzido por um `IrrigationProto::encode*`.

- [ ] **Step 4: Compilar e rodar toda a suíte**

Run: `./bin/run-tests.sh`
Expected: GREEN (exit 0). Nenhum teste existente quebrou; comportamento de envio preservado (mesmo destino/seq), agora com prioridade e gate.

- [ ] **Step 5: Verificar prioridade em envio real (inspeção)**

Confirmar por leitura que `sendHeartbeat` (linha ~723) e o emissor de comando P2P-fallback (linha ~1139) agora passam por `txPacket`. Rodar `grep -n "service->sendToMesh(p, RX_SRC_LOCAL, false)" src/modules/irrigation/IrrigationModule.cpp` — esperado: nenhum resultado remanescente de emissor IrrigationProto.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): choke point txPacket aplica prioridade por tipo + gate de airtime"
```

---

### Task 3: Heartbeat adaptativo — backoff + jitter

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (bloco de agendamento do HB, ~1064-1076; re-seed no epoch)
- Modify: `src/modules/irrigation/IrrigationModule.h` (campo `uint32_t hbJitterMs` se necessário)

**Interfaces:**
- Consumes: `IrrigationAirtime::hbBackoffFactor`, `hbWindowMs`, `hbJitterOffsetMs` (Task 1); `airTime->channelUtilizationPercent()`; `nodeDB->getNodeNum()` (nodeNum local); `settings.configEpoch`, `settings.hbMinutes`. Os knobs `hbTrafficFlags` chegam na Task 4 — esta task assume backoff/jitter sempre ON; a Task 4 injeta a leitura dos flags no mesmo bloco.
- Produces: agendamento de HB com intervalo efetivo e offset de jitter.

- [ ] **Step 1: Adicionar campo de offset de jitter no header**

Em `IrrigationModule.h`, junto a `lastHeartbeatMs`:

```cpp
    uint32_t hbJitterMs = 0;         // offset de jitter atual (reavaliado no epoch)
    uint32_t hbSeedEpoch = 0xFFFFFFFF; // epoch com que hbJitterMs foi semeado
```

- [ ] **Step 2: Reescrever o bloco de agendamento**

Substituir o bloco atual (linhas ~1073-1076):

```cpp
    if (!Throttle::isWithinTimespanMs(lastHeartbeatMs, (uint32_t)settings.hbMinutes * 60 * 1000)) {
        lastHeartbeatMs = millis();
        sendHeartbeat();
    }
```

por:

```cpp
    {
        uint32_t baseMs = (uint32_t)settings.hbMinutes * 60u * 1000u;
        float chUtil = airTime ? airTime->channelUtilizationPercent() : 0.0f;
        uint8_t factor = IrrigationAirtime::hbBackoffFactor(chUtil);
        uint32_t effectiveMs = baseMs * factor;

        // Re-semeia o jitter quando o epoch muda (descorrelaciona flood de cena).
        if (settings.configEpoch != hbSeedEpoch) {
            hbSeedEpoch = settings.configEpoch;
            uint32_t win = IrrigationAirtime::hbWindowMs(effectiveMs);
            hbJitterMs = IrrigationAirtime::hbJitterOffsetMs(nodeDB->getNodeNum(), settings.configEpoch, win);
        }

        if (!Throttle::isWithinTimespanMs(lastHeartbeatMs, effectiveMs + hbJitterMs)) {
            lastHeartbeatMs = millis();
            sendHeartbeat();
        }
    }
```

Observação: o gate de airtime do próprio HB já é aplicado por `txPacket` (Task 2) dentro de `sendHeartbeat`; aqui só ajustamos a cadência. `earlyHeartbeatDue` (linha ~1064) permanece intacto — muda de sensor reporta sem backoff, ainda sujeito ao gate.

- [ ] **Step 3: Compilar e rodar a suíte**

Run: `./bin/run-tests.sh`
Expected: GREEN. Confirmar que `nodeDB->getNodeNum()` é acessível no contexto (é global `extern NodeDB *nodeDB`; incluir `mesh/NodeDB.h` se ainda não incluído em IrrigationModule.cpp — provavelmente já está).

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.cpp src/modules/irrigation/IrrigationModule.h
git commit -m "feat(irrigation): heartbeat adaptativo (backoff por channel-util + jitter por epoch)"
```

---

### Task 4: Knobs de config `hbTrafficFlags` (backward-compat)

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h` (renomear `pad3` → `hbTrafficFlags` + bits + acessores)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (respeitar os flags no agendamento — Task 3)

**Interfaces:**
- Consumes: nada novo.
- Produces: `settings.hbTrafficFlags`, `settings.hbBackoffEnabled()`, `settings.hbJitterEnabled()`.

- [ ] **Step 1: Renomear pad3 e adicionar acessores**

Em `IrrigationSettings.h`, substituir a linha:

```cpp
    uint8_t pad3 = 0;               // padding explícito p/ sizeof múltiplo de 4 (offset 207)
```

por:

```cpp
    // v8+ (reuso de pad @207, ABI preservada): bits de tráfego de airtime.
    // Semântica INVERTIDA (0 = habilitado) p/ blobs antigos (pad3=0) já significarem tudo ON.
    static constexpr uint8_t HBFLAG_DISABLE_BACKOFF = 1 << 0;
    static constexpr uint8_t HBFLAG_DISABLE_JITTER = 1 << 1;
    uint8_t hbTrafficFlags = 0; // @207 — reuso do antigo pad3
    bool hbBackoffEnabled() const { return (hbTrafficFlags & HBFLAG_DISABLE_BACKOFF) == 0; }
    bool hbJitterEnabled() const { return (hbTrafficFlags & HBFLAG_DISABLE_JITTER) == 0; }
```

- [ ] **Step 2: Confirmar que a ABI não mudou**

Run: `./bin/run-tests.sh`
Expected: GREEN — os `static_assert` de tamanho/offset em `IrrigationSettings.h` continuam válidos (campo tem o mesmo tamanho e offset que `pad3`). Se algum `static_assert` de `sizeof` disparar, o rename foi incorreto — reverter e revisar.

- [ ] **Step 3: Respeitar os flags no agendamento**

Em `IrrigationModule.cpp`, no bloco reescrito na Task 3, aplicar os knobs:

```cpp
        uint8_t factor = settings.hbBackoffEnabled() ? IrrigationAirtime::hbBackoffFactor(chUtil) : 1;
        uint32_t effectiveMs = baseMs * factor;

        if (settings.configEpoch != hbSeedEpoch) {
            hbSeedEpoch = settings.configEpoch;
            uint32_t win = settings.hbJitterEnabled() ? IrrigationAirtime::hbWindowMs(effectiveMs) : 0;
            hbJitterMs = IrrigationAirtime::hbJitterOffsetMs(nodeDB->getNodeNum(), settings.configEpoch, win);
        }
```

(`hbJitterOffsetMs` com `win==0` retorna 0 → sem jitter.)

- [ ] **Step 4: Compilar e rodar a suíte**

Run: `./bin/run-tests.sh`
Expected: GREEN.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationSettings.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): knobs hbTrafficFlags (backoff/jitter on por default, reuso de pad @207)"
```

---

### Task 5: Telemetria nativa reduzida nos papéis de irrigação

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (setup/init, região ~266-431 onde o papel é avaliado)

**Interfaces:**
- Consumes: `moduleConfig.telemetry`, `settings.role`.
- Produces: intervalos de telemetria nativa altos em RAM quando o papel é de irrigação.

**IMPORTANTE (blast radius):** override apenas em RAM no boot, SEM persistir (`nodeDB->saveToDisk` NÃO é chamado aqui). O heartbeat de irrigação já carrega vbat/estados/sensores; device_metrics nativo é redundante. Só aplica se o usuário não tiver configurado explicitamente um intervalo — critério: aplicar apenas quando o intervalo atual for 0 (não configurado) ou o default do firmware. Confirmar com o revisor se o comportamento desejado é este antes de finalizar.

- [ ] **Step 1: Localizar o ponto de init pós-load de settings**

Ler `IrrigationModule.cpp` ~256-300 (onde o papel é lido no construtor/setup). Identificar um ponto executado uma vez no boot após `settings` estar carregado e `moduleConfig` disponível.

- [ ] **Step 2: Aplicar override condicional**

Adicionar, nesse ponto de init, para papéis ESTACAO/GATEWAY/REPETIDOR:

```cpp
    // Telemetria nativa é redundante com o heartbeat de irrigação; alonga o intervalo
    // em RAM (não persiste) para desocupar airtime. Só quando não configurado pelo usuário.
    {
        IrrigationRole r = (IrrigationRole)settings.role;
        bool irrigRole = (r == IrrigationRole::ESTACAO || r == IrrigationRole::GATEWAY || r == IrrigationRole::REPETIDOR);
        if (irrigRole) {
            const uint32_t kLongIntervalS = 24u * 60u * 60u; // 24 h
            if (moduleConfig.telemetry.device_update_interval == 0 ||
                moduleConfig.telemetry.device_update_interval < kLongIntervalS)
                moduleConfig.telemetry.device_update_interval = kLongIntervalS;
            if (moduleConfig.telemetry.environment_update_interval != 0 &&
                moduleConfig.telemetry.environment_update_interval < kLongIntervalS)
                moduleConfig.telemetry.environment_update_interval = kLongIntervalS;
        }
    }
```

- [ ] **Step 3: Compilar e rodar a suíte**

Run: `./bin/run-tests.sh`
Expected: GREEN. (Sem teste unitário dedicado — segue o padrão do módulo, cuja lógica testável foi extraída para funções puras. Verificação é por inspeção + build.)

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): alonga telemetria nativa em RAM nos papeis de irrigacao (desocupa airtime)"
```

---

## Notas de verificação final

- `./bin/run-tests.sh` GREEN após cada task e ao final.
- `trunk fmt` nas linhas alteradas (ou clang-format@16.0.3 — ver `trunk-fmt-windows-workaround`).
- Task 5 tem a maior incerteza de produto (mexe em `moduleConfig` global). Revisar com atenção; se indesejado, cortar Task 5 sem impacto nas Tasks 1-4.
