# Modo Remoto P2P — fallback direto quando o gateway cai — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Quando o gateway não responde, a botoeira envia o comando **direto ao nó alvo**, que inverte a própria saída — sem depender do gateway. Caminho gateway-central inalterado quando o gateway está no ar.

**Architecture:** Botoeira arma um timer ao enviar `REMOTE_TRIGGER`; sem `REMOTE_LED` dentro de `T_FALLBACK`, reenvia como `CmdValvula`/`CmdGpo` com **`ACTION_TOGGLE=2`** direto ao nó alvo compilado. O alvo inverte a própria saída (dono do estado + teto de duração). O gateway auto-compila a rota de fallback `{node, kind, outputId}` no `SET_CONFIG` da botoeira quando a zona é de nó único. 100% aditivo (protocolo VERSION=1; settings ABI v7→v8).

**Tech Stack:** C++ (Arduino/ESP32 + native PlatformIO tests), Unity test framework. Spec: `docs/superpowers/specs/2026-08-12-irrigacao-modo-remoto-p2p-fallback-design.md`.

**Testes nativos (Windows):** este box roda a suíte nativa **via Docker** (`Dockerfile.test` + volume `pio-build`), não direto. Comando lógico por suíte: `./bin/run-tests.sh -f <suite>` (exit 0 GREEN). Rode dentro do container. Build ESP32 é CI-only.

---

## File Structure

- **`src/modules/irrigation/IrrigationProtocol.h`** — novo `ACTION_TOGGLE = 2` (constante). Sem mudança de wire (`action` já é `uint8`).
- **`src/modules/irrigation/RemoteFallbackTracker.h`** *(novo)* — unidade pura: arma/limpa/expira o pending por entrada-botão. Native-tested.
- **`src/modules/irrigation/RemoteButtonEdge.h`** — anexa `compileFallbackRoute()` (pura) + reusa `remoteToggleAction()` já existente.
- **`src/modules/irrigation/IrrigationSettings.h` / `.cpp`** — ABI v7→v8: rota de fallback por entrada-botão + `remoteFallbackMs`; migração + static_asserts.
- **`src/modules/irrigation/IrrigationModule.cpp` / `.h`** — cola: aplicar `ACTION_TOGGLE` nos handlers de saída; armar tracker ao disparar trigger; `runOnce` expira → emite comando direto; limpar no `REMOTE_LED`; casar ACK direto → LED; gateway compila rota no `SET_CONFIG`; bump `APP_FW_VERSION`.
- **`src/modules/irrigation/GatewayTables.h`** (ou onde vive `StationEntry`) — ripple do tamanho do blob 184→207.
- **Testes:** `test/test_irrigation_protocol/`, `test/test_remote_button/`, `test/test_irrigation_config/`, e nova suíte `test/test_irrigation_fallback/`.

---

## Task 1: Protocolo — `ACTION_TOGGLE = 2`

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h` (após `struct CmdGpo`, ~linha 83)
- Test: `test/test_irrigation_protocol/test_main.cpp`

- [ ] **Step 1: Write the failing test**

Adicione ao fim de `test/test_irrigation_protocol/test_main.cpp` (antes do `main`/runner) e registre no runner (`RUN_TEST(...)`):

```cpp
void test_cmdValvula_toggle_action_roundtrip()
{
    using namespace IrrigationProto;
    uint8_t buf[32];
    CmdValvula in{3, ACTION_TOGGLE, 0};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 42, in);
    TEST_ASSERT_TRUE(n > 0);
    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_CMD_VALVULA, h.type);
    CmdValvula out;
    TEST_ASSERT_TRUE(decodeCmdValvula(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(3, out.valveId);
    TEST_ASSERT_EQUAL_UINT8(ACTION_TOGGLE, out.action);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_irrigation_protocol`
Expected: FAIL — `ACTION_TOGGLE` não declarado.

- [ ] **Step 3: Write minimal implementation**

Em `IrrigationProtocol.h`, logo após `enum AckStatus ...` (ou junto às constantes de topo do namespace):

```cpp
// Ação de comando de saída (CmdValvula/CmdGpo): 0 = fechar, 1 = abrir.
// 2 = TOGGLE: o nó alvo inverte a própria saída (modo remoto P2P fallback).
constexpr uint8_t ACTION_TOGGLE = 2;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_irrigation_protocol`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.h test/test_irrigation_protocol/test_main.cpp
git commit -m "feat(irrigation): ACTION_TOGGLE=2 para comando de saída (P2P fallback)"
```

---

## Task 2: `RemoteFallbackTracker` — unidade pura (arm / clear / expire)

**Files:**
- Create: `src/modules/irrigation/RemoteFallbackTracker.h`
- Create: `test/test_irrigation_fallback/test_main.cpp`
- Create: `test/test_irrigation_fallback/` (nova suíte; espelhe a estrutura de `test/test_remote_button/`)

Semântica: uma entrada por índice de botão (0..3). `arm(i, nowMs)` marca pendente com prazo `nowMs + windowMs`. `clear(i)` cancela (chegou `REMOTE_LED` → gateway no ar). `takeExpired(nowMs, out[])` devolve os índices cujo prazo passou **e** ainda armados, limpando-os (dispara uma única vez — fire-and-forget).

- [ ] **Step 1: Write the failing test**

`test/test_irrigation_fallback/test_main.cpp`:

```cpp
#include <unity.h>
#include "../../src/modules/irrigation/RemoteFallbackTracker.h"

void test_arm_then_clear_before_timeout_no_fire()
{
    RemoteFallbackTracker t(2000);
    t.arm(1, 1000);
    t.clear(1); // REMOTE_LED chegou
    uint8_t out[4];
    TEST_ASSERT_EQUAL_UINT(0, t.takeExpired(4000, out, 4));
}

void test_arm_expires_fires_once()
{
    RemoteFallbackTracker t(2000);
    t.arm(2, 1000);
    uint8_t out[4];
    TEST_ASSERT_EQUAL_UINT(0, t.takeExpired(2500, out, 4)); // 1000+2000=3000, ainda não
    TEST_ASSERT_EQUAL_UINT(1, t.takeExpired(3001, out, 4));
    TEST_ASSERT_EQUAL_UINT8(2, out[0]);
    TEST_ASSERT_EQUAL_UINT(0, t.takeExpired(9000, out, 4)); // não redispara
}

void test_rearm_same_index_resets_deadline()
{
    RemoteFallbackTracker t(2000);
    t.arm(0, 1000);
    t.arm(0, 5000); // repressionou antes de expirar → novo prazo
    uint8_t out[4];
    TEST_ASSERT_EQUAL_UINT(0, t.takeExpired(6000, out, 4));
    TEST_ASSERT_EQUAL_UINT(1, t.takeExpired(7001, out, 4));
}

void setUp() {}
void tearDown() {}
int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_arm_then_clear_before_timeout_no_fire);
    RUN_TEST(test_arm_expires_fires_once);
    RUN_TEST(test_rearm_same_index_resets_deadline);
    return UNITY_END();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_irrigation_fallback`
Expected: FAIL — header inexistente.

- [ ] **Step 3: Write minimal implementation**

`src/modules/irrigation/RemoteFallbackTracker.h`:

```cpp
#pragma once
#include <stdint.h>
#include "IrrigationSettings.h" // MAX_DIGITAL_IN

// Rastreia, por entrada-botão, um gatilho pendente que vira comando direto (P2P
// fallback) se o gateway não confirmar (REMOTE_LED) dentro de windowMs. Puro.
class RemoteFallbackTracker {
  public:
    explicit RemoteFallbackTracker(uint32_t windowMs) : window(windowMs) {}
    void setWindow(uint32_t windowMs) { window = windowMs; }

    void arm(uint8_t idx, uint32_t nowMs)
    {
        if (idx >= IrrigationSettings::MAX_DIGITAL_IN)
            return;
        slots[idx].armed = true;
        slots[idx].dueMs = nowMs + window;
    }
    void clear(uint8_t idx)
    {
        if (idx < IrrigationSettings::MAX_DIGITAL_IN)
            slots[idx].armed = false;
    }
    // Preenche out[] com índices expirados (dispara 1×), limpando-os. Retorna a contagem.
    size_t takeExpired(uint32_t nowMs, uint8_t *out, size_t cap)
    {
        size_t k = 0;
        for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
            if (slots[i].armed && (int32_t)(nowMs - slots[i].dueMs) >= 0) {
                slots[i].armed = false;
                if (k < cap)
                    out[k++] = i;
            }
        }
        return k;
    }

  private:
    struct Slot {
        bool armed = false;
        uint32_t dueMs = 0;
    };
    uint32_t window;
    Slot slots[IrrigationSettings::MAX_DIGITAL_IN];
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_irrigation_fallback`
Expected: PASS (3 testes).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/RemoteFallbackTracker.h test/test_irrigation_fallback/
git commit -m "feat(irrigation): RemoteFallbackTracker (unidade pura, P2P fallback)"
```

---

## Task 3: `compileFallbackRoute` — deriva a rota do gateway (pura)

**Files:**
- Modify: `src/modules/irrigation/RemoteButtonEdge.h` (anexar helper puro)
- Test: `test/test_remote_button/test_main.cpp`

Regra: dada uma associação e a zona-alvo, se a zona é de **nó único** (`node != 0`) e o alvo entende `ACTION_TOGGLE` (`fwOk`), devolve `{node, kind, outputId}` válido. Zona ausente/grupo ou `fwOk==false` ⇒ rota vazia (`node==0`). `kind`: 0 = válvula, 1 = GPO (deriva de `zoneTipo`: 0 = válvula, 1 = GPO — mesma convenção de `Zone.tipo`).

- [ ] **Step 1: Write the failing test**

Adicione a `test/test_remote_button/test_main.cpp` e registre no runner:

```cpp
void test_compileFallbackRoute_single_node()
{
    FallbackRoute r = compileFallbackRoute(/*zoneNode=*/0x11223344, /*zoneIndex=*/2,
                                           /*zoneTipo=*/0, /*isGroup=*/false, /*fwOk=*/true);
    TEST_ASSERT_EQUAL_UINT32(0x11223344, r.node);
    TEST_ASSERT_EQUAL_UINT8(0, r.kind);       // válvula
    TEST_ASSERT_EQUAL_UINT8(2, r.outputId);
}
void test_compileFallbackRoute_group_is_empty()
{
    FallbackRoute r = compileFallbackRoute(0x11223344, 2, 0, /*isGroup=*/true, true);
    TEST_ASSERT_EQUAL_UINT32(0, r.node);
}
void test_compileFallbackRoute_old_fw_is_empty()
{
    FallbackRoute r = compileFallbackRoute(0x11223344, 2, 0, false, /*fwOk=*/false);
    TEST_ASSERT_EQUAL_UINT32(0, r.node);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_remote_button`
Expected: FAIL — `FallbackRoute`/`compileFallbackRoute` indefinidos.

- [ ] **Step 3: Write minimal implementation**

Ao fim de `src/modules/irrigation/RemoteButtonEdge.h`:

```cpp
// Rota de fallback P2P compilada pelo gateway para uma entrada-botão.
struct FallbackRoute {
    uint32_t node = 0;   // 0 = sem fallback (zona grupo / alvo fw antigo / zona ausente)
    uint8_t kind = 3;    // 0 = válvula, 1 = GPO, 3 = nenhum
    uint8_t outputId = 0;
};

// zoneTipo: 0 = válvula, 1 = GPO (convenção de Zone.tipo). isGroup = zona multi-nó.
// fwOk = alvo reporta APP_FW_VERSION que entende ACTION_TOGGLE.
inline FallbackRoute compileFallbackRoute(uint32_t zoneNode, uint8_t zoneIndex, uint8_t zoneTipo,
                                          bool isGroup, bool fwOk)
{
    FallbackRoute r;
    if (zoneNode == 0 || isGroup || !fwOk)
        return r; // vazia
    r.node = zoneNode;
    r.kind = (zoneTipo == 1) ? 1u : 0u;
    r.outputId = zoneIndex;
    return r;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_remote_button`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/RemoteButtonEdge.h test/test_remote_button/test_main.cpp
git commit -m "feat(irrigation): compileFallbackRoute (pura) p/ rota P2P fallback"
```

---

## Task 4: Settings ABI v7→v8 (struct + asserts + migração)

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h`
- Modify: `src/modules/irrigation/IrrigationSettings.cpp` (migração)
- Test: `test/test_irrigation_config/test_main.cpp`

Layout do apêndice v8 (no FIM, offset 184). **Alinhamento**: a struct tem alinhamento 4 (membros uint32), então `sizeof` precisa ser múltiplo de 4 e o `uint16` precisa de offset par. Ordem escolhida: `btnFallbackNode[4]` (uint32×4 = 16 B, offset 184) · `btnFallbackOutId[4]` (uint8×4 = 4 B, offset 200) · `remoteFallbackMs` (uint16, offset 204, par ✓) · `btnFallbackKind` (uint8, offset 206: 2 bits/entrada, default `0xFF` = tudo "nenhum") · `pad3` (uint8 explícito, offset 207) = **total 208 B**.

- [ ] **Step 1: Write the failing test**

Adicione a `test/test_irrigation_config/test_main.cpp` e registre no runner:

```cpp
void test_migrate_v7_to_v8_preserves_prefix_and_defaults()
{
    IrrigationSettings src; // v7-shaped, mas struct atual = v8
    src.version = 7;
    src.boundGateway = 0xDEADBEEF;
    src.digitalInBtnMask = 0x05;
    uint8_t raw[IRRIGATION_SETTINGS_V7_SIZE];
    memcpy(raw, &src, IRRIGATION_SETTINGS_V7_SIZE); // só o prefixo v7
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, IRRIGATION_SETTINGS_V7_SIZE, out));
    TEST_ASSERT_EQUAL_UINT16(8, out.version);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, out.boundGateway);
    TEST_ASSERT_EQUAL_UINT8(0x05, out.digitalInBtnMask);
    TEST_ASSERT_EQUAL_UINT32(0, out.btnFallbackNode[0]); // default = sem fallback
    TEST_ASSERT_EQUAL_UINT8(0xFF, out.btnFallbackKind);
}
void test_migrate_v8_exact_roundtrip()
{
    IrrigationSettings src;
    src.version = 8;
    src.btnFallbackNode[2] = 0x0A0B0C0D;
    src.btnFallbackOutId[2] = 5;
    src.remoteFallbackMs = 1500;
    uint8_t raw[sizeof(IrrigationSettings)];
    memcpy(raw, &src, sizeof(raw));
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_UINT32(0x0A0B0C0D, out.btnFallbackNode[2]);
    TEST_ASSERT_EQUAL_UINT16(1500, out.remoteFallbackMs);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_irrigation_config`
Expected: FAIL — campos/`IRRIGATION_SETTINGS_V7_SIZE` inexistentes; `version` default != 8.

- [ ] **Step 3: Write minimal implementation**

Em `IrrigationSettings.h`:

1. `uint16_t version = 8;` (era 7).
2. Após `uint8_t digitalInLedIdx = 0xFF;` acrescente:

```cpp
    // v8 (Modo Remoto P2P fallback): rota direta por entrada-botão, auto-compilada
    // pelo gateway; usada quando o gateway não confirma o gatilho a tempo. Apêndice no FIM.
    uint32_t btnFallbackNode[MAX_DIGITAL_IN] = {0, 0, 0, 0}; // 0 = sem fallback
    uint8_t btnFallbackOutId[MAX_DIGITAL_IN] = {0, 0, 0, 0};  // id local da saída no nó alvo
    uint16_t remoteFallbackMs = 0;  // T_FALLBACK; 0 => default compilado (REMOTE_FALLBACK_DEFAULT_MS)
    uint8_t btnFallbackKind = 0xFF; // 2 bits/entrada: 0=válvula,1=GPO,3=nenhum (default tudo nenhum)
    uint8_t pad3 = 0;               // padding explícito p/ sizeof múltiplo de 4 (offset 207)
```

3. Acrescente `static constexpr size_t IRRIGATION_SETTINGS_V7_SIZE = 184;` junto aos outros.
4. Acrescente helpers e default:

```cpp
constexpr uint16_t REMOTE_FALLBACK_DEFAULT_MS = 5000; // 5 s (T_FALLBACK, decisão do usuário)
inline uint8_t btnFallbackKindOf(const IrrigationSettings &s, uint8_t i)
{
    return (uint8_t)((s.btnFallbackKind >> (2u * i)) & 0x3u);
}
```

5. Atualize os static_asserts finais:

```cpp
static_assert(offsetof(IrrigationSettings, btnFallbackNode) == 184, "ABI v8");
static_assert(offsetof(IrrigationSettings, btnFallbackOutId) == 200, "ABI v8");
static_assert(offsetof(IrrigationSettings, remoteFallbackMs) == 204, "ABI v8");
static_assert(offsetof(IrrigationSettings, btnFallbackKind) == 206, "ABI v8");
static_assert(sizeof(IrrigationSettings) == 208, "on-disk settings format is ABI-dependent; bump version on layout change");
```

(Remova o `static_assert(sizeof(...) == 184 ...)` antigo e mantenha os asserts de v4..v7 intactos.)

6. Atualize o comentário de layout do topo (bloco `// v7 ...`) adicionando: `// --- v8 (P2P fallback) --- 184 btnFallbackNode[4](16) | 200 btnFallbackOutId[4](4) | 204 remoteFallbackMs(2) | 206 btnFallbackKind(1) | 207 pad3(1) → 208`.

Em `IrrigationSettings.cpp` (`migrateIrrigationSettings`):

- Troque o bloco `if (version == 7)` para **migrar** (não exact-copy):

```cpp
    if (version == 8) {
        if (n != sizeof(IrrigationSettings))
            return false;
        memcpy(&out, raw, sizeof(out));
        return true;
    }
    if (version == 7) {
        if (n != IRRIGATION_SETTINGS_V7_SIZE)
            return false;
        IrrigationSettings s; // defaults v8 (campos de fallback zerados/0xFF)
        memcpy(&s, raw, IRRIGATION_SETTINGS_V7_SIZE); // v7 é prefixo do v8
        s.version = 8;
        out = s;
        return true;
    }
```

- Em **todos** os blocos v6..v1, troque `s.version = 7;` por `s.version = 8;` (o alvo agora é v8; comentários "prefixo do v7" → "prefixo do v8").

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_irrigation_config`
Expected: PASS (incl. os testes v7→v8 novos e os de migração pré-existentes).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationSettings.h src/modules/irrigation/IrrigationSettings.cpp test/test_irrigation_config/test_main.cpp
git commit -m "feat(irrigation): settings ABI v7->v8 (rota de fallback P2P por botoeira)"
```

---

## Task 5: Handlers de saída aplicam `ACTION_TOGGLE` (alvo inverte a própria saída)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (`handleCmdValvula` ~447-483, `handleCmdGpo` ~505-528)

O alvo, ao receber `action==ACTION_TOGGLE`, lê o estado local e comanda o inverso via `remoteToggleAction()` (já em `RemoteButtonEdge.h`). Ligar herda o teto `MAX_OPEN_SECONDS`/`maxOpenConfigS` como já ocorre.

- [ ] **Step 1: Escrever a mudança (válvula)**

Em `handleCmdValvula`, **antes** do bloco `if (safeMode && cmd.action == 1)`, normalize o toggle:

```cpp
    // P2P fallback: TOGGLE resolvido no alvo — inverte a saída local.
    if (cmd.action == IrrigationProto::ACTION_TOGGLE)
        cmd.action = remoteToggleAction(valves.isOpen(cmd.valveId));
```

(Assim o resto — safeMode, open/close, ACK, auditoria — funciona inalterado, já com `action` 0/1.)

- [ ] **Step 2: Escrever a mudança (GPO)**

Em `handleCmdGpo`, **antes** do bloco `if (safeMode && cmd.action == 1)`:

```cpp
    if (cmd.action == IrrigationProto::ACTION_TOGGLE)
        cmd.action = remoteToggleAction(gpos.isOn(cmd.gpoId));
```

Garanta `#include "RemoteButtonEdge.h"` no topo de `IrrigationModule.cpp` (provavelmente já incluído; confirme).

- [ ] **Step 3: Build nativo do módulo**

Run: `./bin/run-tests.sh -f test_irrigation_valve` (a suíte linka o módulo; confirme que compila) — ou o build nativo do módulo usado nas fases anteriores.
Expected: compila/linka; suíte GREEN.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): alvo aplica ACTION_TOGGLE (inverte saída local)"
```

---

## Task 6: Cola da estação — armar tracker, expirar→comando direto, limpar no REMOTE_LED, ACK direto→LED

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (membros novos)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (loop da botoeira ~1055-1082; `runOnce`; `handleRemoteLed` ~4102; handler de ACK)

Esta task é **cola de módulo** (não coberta por unidade pura — padrão das fases anteriores). Verificação = build nativo + banca.

- [ ] **Step 1: Membros novos em `IrrigationModule.h`**

Na seção privada do `IrrigationModule` (perto de `_btnEdge`/`_remoteLed`):

```cpp
    RemoteFallbackTracker _fallback{REMOTE_FALLBACK_DEFAULT_MS};
    // casamento ACK direto → LED do fallback P2P: seq do comando direto emitido por slot de LED.
    struct PendingDirect { uint32_t seq = 0; uint8_t ledSlot = 3; bool armed = false; };
    PendingDirect _pendingDirect[IrrigationSettings::MAX_DIGITAL_IN];
```

Inclua os headers: `#include "RemoteFallbackTracker.h"` no topo.

- [ ] **Step 2: Armar o tracker ao disparar o trigger**

No loop da botoeira (`runOnce`, ~1066-1081), dentro do `if (_btnEdge[i].update(...))`, **depois** de enviar o `REMOTE_TRIGGER`, arme o fallback só se houver rota compilada:

```cpp
            if (settings.btnFallbackNode[i] != 0) {
                _fallback.setWindow(settings.remoteFallbackMs ? settings.remoteFallbackMs
                                                              : REMOTE_FALLBACK_DEFAULT_MS);
                _fallback.arm(i, millis());
            }
```

- [ ] **Step 3: Expirar → emitir comando direto (novo bloco no `runOnce`)**

Logo após o loop da botoeira (antes de `refreshLedMode();`):

```cpp
    // P2P fallback: gatilhos sem REMOTE_LED a tempo → comando direto ao nó alvo.
    {
        uint8_t exp[IrrigationSettings::MAX_DIGITAL_IN];
        size_t k = _fallback.takeExpired(millis(), exp, IrrigationSettings::MAX_DIGITAL_IN);
        for (size_t j = 0; j < k; j++) {
            uint8_t i = exp[j];
            if (settings.btnFallbackNode[i] == 0)
                continue;
            uint8_t kind = btnFallbackKindOf(settings, i); // 0=válvula,1=GPO
            meshtastic_MeshPacket *p = allocDataPacket();
            p->to = settings.btnFallbackNode[i];
            uint32_t seq = ++txSeq;
            if (kind == 1) {
                IrrigationProto::CmdGpo c{settings.btnFallbackOutId[i], IrrigationProto::ACTION_TOGGLE, 0};
                p->decoded.payload.size = (uint16_t)IrrigationProto::encodeCmdGpo(
                    p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), seq, c);
            } else {
                IrrigationProto::CmdValvula c{settings.btnFallbackOutId[i], IrrigationProto::ACTION_TOGGLE, 0};
                p->decoded.payload.size = (uint16_t)IrrigationProto::encodeCmdValvula(
                    p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), seq, c);
            }
            if (p->decoded.payload.size) {
                service->sendToMesh(p, RX_SRC_LOCAL, false);
                uint8_t slot = digitalInLedSlot(settings, i);
                if (slot <= 1) {
                    _pendingDirect[i] = {seq, slot, true};
                }
            } else {
                packetPool.release(p);
            }
        }
    }
```

- [ ] **Step 4: Limpar o tracker quando `REMOTE_LED` chega (gateway no ar)**

Em `handleRemoteLed` (~4110), após aplicar os estados de LED, cancele todos os fallbacks pendentes (o gateway respondeu):

```cpp
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++)
        _fallback.clear(i);
```

- [ ] **Step 5: ACK direto do alvo → dirige o LED**

No handler de `MSG_ACK` da estação (procure `decodeAck`/`handleAck`), após decodar o `Ack`, case pelo `ackedSeq` contra `_pendingDirect` e dirija o `RemoteLedFsm`:

```cpp
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
        if (_pendingDirect[i].armed && _pendingDirect[i].seq == ack.ackedSeq) {
            bool ok = (ack.status == IrrigationProto::ACK_OK);
            uint8_t slot = _pendingDirect[i].ledSlot;
            if (slot <= 1)
                _remoteLed[slot].onLedState(ok, millis()); // SOLID no OK, OFF no NACK
            _pendingDirect[i].armed = false;
        }
    }
```

(Se não existir handler de ACK na estação, adicione um mínimo no dispatch de `MSG_ACK` que só faz esse casamento — o ACK do modo normal é consumido no gateway; na estação, o ACK direto é a confirmação do fallback.)

- [ ] **Step 6: Build nativo + `node --check` não se aplica**

Run: `./bin/run-tests.sh` (suíte inteira; confirme GREEN e que o módulo linka).
Expected: GREEN. (Comportamento de rádio validado em banca — ver Plano de Testes.)

- [ ] **Step 7: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): estação emite comando direto P2P no timeout do gatilho + LED por ACK direto"
```

---

## Task 7: Gateway compila a rota de fallback no `SET_CONFIG` + bump `APP_FW_VERSION`

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h` (`APP_FW_VERSION` 0x0800→0x0900)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (onde o gateway monta o blob de `SET_CONFIG` por estação — ver `gwBuild*`/`buildStationConfigBlob`; procure `encodeSetConfig` no contexto gateway, ~3736)

Ao montar a config de uma estação com botoeira, para cada entrada-botão `i` daquela estação que é gatilho de alguma associação, compile `{btnFallbackNode[i], btnFallbackOutId[i], btnFallbackKind}` via `compileFallbackRoute(...)` a partir da `Zone` alvo e da flag de fw do nó alvo (`APP_FW_VERSION` reportado no heartbeat/survey ≥ 0x0900). Zona grupo / fw antigo ⇒ campos zerados.

- [ ] **Step 1: Bump `APP_FW_VERSION`**

Em `IrrigationProtocol.h`: `constexpr uint16_t APP_FW_VERSION = 0x0900;` (era 0x0800).

- [ ] **Step 2: Compilar a rota no blob da estação**

No ponto onde o gateway preenche o `IrrigationSettings` da estação antes de `encodeSetConfig` (a partir do `StationEntry.blob` migrado), acrescente:

```cpp
    // P2P fallback: compila a rota direta por entrada-botão desta estação.
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
        cfg.btnFallbackNode[i] = 0;
        cfg.btnFallbackKind = (uint8_t)(cfg.btnFallbackKind | (0x3u << (2 * i))); // "nenhum"
        const RemoteAssoc *hits[RemoteButtonTable::MAX];
        size_t k = gateway.remoteButtons.findByTrigger(stationNode, i, hits, RemoteButtonTable::MAX);
        if (k == 0)
            continue;
        const Zone *z = gateway.zones.byId(hits[0]->targetZoneId);
        if (!z)
            continue;
        bool isGroup = /* zona pertence a grupo multi-nó */ gateway.zoneIsGroup(z->id);
        bool fwOk = gwTargetFwSupportsToggle(z->node); // APP_FW_VERSION>=0x0900 no heartbeat/survey
        FallbackRoute r = compileFallbackRoute(z->node, z->index, z->tipo, isGroup, fwOk);
        cfg.btnFallbackNode[i] = r.node;
        cfg.btnFallbackOutId[i] = r.outputId;
        cfg.btnFallbackKind = (uint8_t)((cfg.btnFallbackKind & ~(0x3u << (2 * i))) |
                                        ((r.node ? r.kind : 0x3u) << (2 * i)));
    }
    cfg.remoteFallbackMs = 0; // usa default; ajustável no futuro
```

Notas para o agente:
- `zoneIsGroup(id)` e `gwTargetFwSupportsToggle(node)` são helpers a criar se não existirem: o primeiro consulta a tabela de grupos hidráulicos (`gateway.hydraulicGroups`/`routeZoneToGroup` já sabe se a zona roteia p/ grupo — reuse essa checagem); o segundo lê o `APP_FW_VERSION` que o `StationEntry`/heartbeat já guarda (procure onde o survey/heartbeat grava fwVersion por estação). Se o fw da estação-alvo não for rastreado, use `false` (conservador: sem fallback) e registre follow-up.
- `stationNode` = o `NodeNum` da estação cujo `SET_CONFIG` está sendo montado (já disponível no laço de build).

- [ ] **Step 3: Build nativo (módulo linka)**

Run: `./bin/run-tests.sh`
Expected: GREEN.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): gateway compila rota de fallback P2P no SET_CONFIG + APP_FW_VERSION 0x0900"
```

---

## Task 8: Ripple ABI — `StationEntry.blob` 184→208 e serialização

**Files:**
- Modify: onde `StationEntry.blob` e `STATION_ENTRY` serialize vivem (procure `blob[184]` / `SERIALIZED_MAX` em `GatewayTables.h`/`StationRegistry*`)
- Modify: `gwBuildBackup` / `importConfigTablesFromBackup` se emitirem campos por-estação byte-a-byte
- Test: `test/test_irrigation_gwtables/test_main.cpp` (se cobrir serialização de estação)

- [ ] **Step 1: Localizar os tamanhos fixos**

Run (Grep): procure `184` e `blob` em `src/modules/irrigation/` — todo literal `184` que representa o tamanho do blob de settings vira `208` (ou, melhor, `sizeof(IrrigationSettings)` / `IRRIGATION_SETTINGS...`). Ajuste `StationEntry.blob[208]`, `STATION_ENTRY` serialize (215→239, +24) e `StationRegistry::SERIALIZED_MAX`.

- [ ] **Step 2: Atualizar/rodar os testes de tabelas**

Run: `./bin/run-tests.sh -f test_irrigation_gwtables`
Expected: ajuste as expectativas de tamanho e passe GREEN.

- [ ] **Step 3: `extractLight` tolera campos v8**

Confirme que `extractLight` (leitura key-seek do blob de estação) ignora chaves desconhecidas — nenhum campo novo é obrigatório na leitura leve. Se houver assert de tamanho, relaxe-o.

- [ ] **Step 4: Suíte inteira**

Run: `./bin/run-tests.sh`
Expected: GREEN (todas as suítes).

- [ ] **Step 5: Commit**

```bash
git add -A src/modules/irrigation/ test/
git commit -m "chore(irrigation): ripple ABI v8 (StationEntry blob 184->207 + serialize)"
```

---

## Task 9: Format + verificação final

- [ ] **Step 1: Format**

Run: `trunk fmt`

- [ ] **Step 2: Suíte nativa completa**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN). Registre a contagem de suítes.

- [ ] **Step 3: Commit do fmt (se houver diff)**

```bash
git add -A
git commit -m "style(irrigation): trunk fmt (P2P fallback)"
```

- [ ] **Step 4: Banca (manual — rádio não é nativo-testável)**

Roteiro (spec §Plano de testes):
1. Gateway no ar: botoeira → saída no outro nó, LED pisca→fixo (caminho normal inalterado).
2. Gateway desligado: botoeira → após `T_FALLBACK` (5 s), comando direto → saída inverte no alvo → ACK direto → LED fixo; nova pressão desliga; alvo aplica teto de duração.
3. Zona = grupo, gateway desligado: pressiona → sem fallback → LED expira p/ apagado.
4. Gateway volta: heartbeat reconcilia o estado togglado em P2P.

---

## Notas de não-regressão (checar ao revisar)

- Protocolo VERSION=1; caminho gateway-central byte-idêntico quando o gateway responde.
- `action` 0/1 intactos; `2` só emitido por gateway/estação novos.
- Modo seguro bloqueia ativação local igual (o toggle vira `open`/`close` antes do check de safeMode).
- Alvo fw antigo ⇒ gateway não compila fallback (checagem `fwOk` na Task 7).
- Nós de firmware antigo ignoram campos v8 (aditivo; `extractLight` key-seek).
