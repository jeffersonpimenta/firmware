# Motor sem-válvula (gateway standalone local + remoto) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Permitir que um motor (saída a relé/GPO) seja acionado como zona normal tanto **localmente na placa do gateway** (standalone, sem válvulas/estações) quanto **remotamente num nó**.

**Architecture:** Interceptar o ponto único `gwSendValveCmd`: quando a zona aponta para o próprio nó do gateway (`Zone.node == nodeDB->getNodeNum()`), acionar os drivers locais (`valves`/`gpos`) em vez de radiar, e confirmar o comando via um ACK sintético drenado no `gwTick` (para o handshake do motor de grupos avançar). O motor reusa `tipo=1` (GPO) — zero mudança de ABI. O painel ganha a opção de nó-alvo "Gateway (local)" e o rótulo "Motor / GPO".

**Tech Stack:** C++11 (firmware Meshtastic/ESP32 + build nativo portduino), Unity (testes nativos), JS vanilla (painel `data/irrigacao/`).

## Global Constraints

- **Sem mudança de ABI:** `Zone` (`IZN2`), `IrrigationSettings`, protocolo VERSION=1 — todos intactos. Mudança 100% aditiva.
- **Motor = zona `tipo=1` (GPO)** acionada temporizada; sem novo `tipo`.
- **Alvo local = `Zone.node == nodeDB->getNodeNum()`** (e `node != 0`).
- **Fail-safe compilado** (`ValveController::MAX_OPEN_SECONDS`, `GpoController` timer) vale igual em local e remoto — o tick já roda p/ todos os papéis no topo do `runOnce`.
- **safeMode bloqueia ativação** (paridade com a estação).
- Testes nativos: `./bin/run-tests.sh` (exit 0 GREEN). Suíte única: `pio test -e native -f <suite>`. No Windows, rodar via Docker (ver `native-test-docker`/workaround do projeto).
- `trunk fmt` antes de cada commit (no host Windows não roda; CI do fork valida — seguir clang-format do repo).
- Módulo-cola (`IrrigationModule.cpp`, endpoints webserver-guarded) **não** é coberto por teste nativo de unidade; verifica-se por **build nativo** + **banca**. CI ESP32 obrigatório antes do merge.

---

### Task 1: Predicado puro `isLocalTarget`

**Files:**
- Modify: `src/modules/irrigation/GatewayTables.h` (adicionar função livre inline após o `struct Zone`)
- Test: `test/test_irrigation_gwtables/test_main.cpp`

**Interfaces:**
- Produces: `inline bool isLocalTarget(uint32_t node, uint32_t selfNode)` — `true` sse `node != 0 && node == selfNode`.

- [ ] **Step 1: Escrever o teste que falha**

Adicionar em `test/test_irrigation_gwtables/test_main.cpp` (nova função + `RUN_TEST` no `setup()`):

```cpp
static void test_is_local_target()
{
    TEST_ASSERT_TRUE(isLocalTarget(0x1234abcd, 0x1234abcd)); // próprio nó
    TEST_ASSERT_FALSE(isLocalTarget(0x1234abcd, 0x0000beef)); // outro nó
    TEST_ASSERT_FALSE(isLocalTarget(0, 0));                    // node==0 nunca é local
    TEST_ASSERT_FALSE(isLocalTarget(0, 0x1234abcd));           // slot vazio
}
```

E registrar no `setup()`:

```cpp
    RUN_TEST(test_is_local_target);
```

- [ ] **Step 2: Rodar o teste e confirmar que falha**

Run: `pio test -e native -f test_irrigation_gwtables`
Expected: FAIL — `isLocalTarget` não declarado.

- [ ] **Step 3: Implementar**

Em `src/modules/irrigation/GatewayTables.h`, logo após o fechamento do `struct Zone { ... };` (antes de `class ZoneTable`):

```cpp
// Uma zona cujo node é o próprio nó do gateway aciona a saída LOCAL (sem rádio).
// node==0 (slot vazio / broadcast) nunca conta como local.
inline bool isLocalTarget(uint32_t node, uint32_t selfNode)
{
    return node != 0 && node == selfNode;
}
```

- [ ] **Step 4: Rodar o teste e confirmar que passa**

Run: `pio test -e native -f test_irrigation_gwtables`
Expected: PASS (todos os casos do suite verdes).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/GatewayTables.h test/test_irrigation_gwtables/test_main.cpp
git commit -m "feat(irrigation): isLocalTarget — predicado puro de zona local do gateway"
```

---

### Task 2: Expor `selfNode` no overview + getter do módulo

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h:56-66` (campo em `OverviewCtx`)
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp:71-86` (`buildOverview` emite `selfNode`)
- Modify: `src/modules/irrigation/IrrigationModule.h` (getter `gwSelfNode()`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (impl do getter)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp:57-66` (`hOverview` preenche `c.selfNode`)
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: nada.
- Produces: JSON de `/api/irrigation/overview` passa a conter `"selfNode":<uint32>`. `uint32_t IrrigationModule::gwSelfNode() const`.

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_webapi/test_main.cpp`, adicionar função + `RUN_TEST`:

```cpp
static void test_overview_emits_self_node()
{
    OverviewCtx c = {};
    c.selfNode = 0xDEADBEEF;
    char buf[512];
    size_t n = buildOverview(c, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"selfNode\":3735928559")); // 0xDEADBEEF decimal
}
```

Registrar no `setup()`:

```cpp
    RUN_TEST(test_overview_emits_self_node);
```

- [ ] **Step 2: Rodar e confirmar falha**

Run: `pio test -e native -f test_irrigation_webapi`
Expected: FAIL — `OverviewCtx` sem membro `selfNode`.

- [ ] **Step 3: Implementar**

`IrrigationWebApi.h`, dentro de `struct OverviewCtx` (após `runningRemainMin`):

```cpp
    uint32_t selfNode = 0; // node do próprio gateway (p/ opção "Gateway (local)" no form de zona)
```

`IrrigationWebApi.cpp`, em `buildOverview`, antes de `w.endObject();`:

```cpp
    w.keyNum("selfNode", (int64_t)ctx.selfNode);
```

`IrrigationModule.h`, junto aos getters `gw*` (ex.: após `gwNodeLabel()`):

```cpp
    uint32_t gwSelfNode() const; // nodeDB->getNodeNum() — usado pelo painel p/ oferecer "Gateway (local)"
```

`IrrigationModule.cpp` (perto de `gwNodeLabel`):

```cpp
uint32_t IrrigationModule::gwSelfNode() const
{
    return nodeDB->getNodeNum();
}
```

`IrrigationWebEndpoints.cpp`, em `hOverview`, após `c.pairingSecondsLeft = ...;`:

```cpp
    c.selfNode = irrigationModule->gwSelfNode();
```

- [ ] **Step 4: Rodar e confirmar passa**

Run: `pio test -e native -f test_irrigation_webapi`
Expected: PASS.

- [ ] **Step 5: Verificar build nativo do módulo**

Run: `pio run -e native`
Expected: SUCCESS (compila/linka `IrrigationModule.cpp` com o novo getter).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp src/modules/irrigation/IrrigationWebEndpoints.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): overview expõe selfNode + gwSelfNode()"
```

---

### Task 3: Extrair `confirmCommand` de `handleGwAck` (refactor)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarar `confirmCommand`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp:3603-3636` (`handleGwAck` delega)

**Interfaces:**
- Produces: `void IrrigationModule::confirmCommand(uint32_t node, uint32_t seq, uint8_t reason, bool ok)` — núcleo comum de confirmação de comando (tracker + motor de grupos + alerta CMD_FAIL). Reutilizado pelo ACK de rádio e pelo drive local (Task 4).

Refactor puro — **sem mudança de comportamento observável**. Sem teste novo; verificação = build + suíte completa verde.

- [ ] **Step 1: Declarar em `IrrigationModule.h`**

Junto aos handlers privados (perto de `handleGwAck`):

```cpp
    // Núcleo de confirmação de comando: tracker + motor de grupos + alerta no NACK.
    // Chamado pelo ACK de rádio (handleGwAck) e pelo drive local (ACK sintético).
    void confirmCommand(uint32_t node, uint32_t seq, uint8_t reason, bool ok);
```

- [ ] **Step 2: Implementar `confirmCommand` e reescrever `handleGwAck`**

Substituir o corpo de `handleGwAck` (`IrrigationModule.cpp:3603-3636`) por:

```cpp
void IrrigationModule::confirmCommand(uint32_t node, uint32_t seq, uint8_t reason, bool ok)
{
    if (!ok) {
        // NACK é resposta definitiva — remove pendência e alerta (decisão §6).
        if (gateway.tracker.onAck(node, seq)) {
            LOG_WARN("Irrigation GW: NACK from 0x%08x seq=%u reason=%u", node, seq, reason);
            Alert a;
            a.type = AlertType::CMD_FAIL;
            a.node = node;
            a.arg = reason; // reason como arg conforme decisão §6
            a.atMs = millis();
            gateway.alerts.push(a);
        }
        // Fase 7a: NACK é FALHA — o motor de grupos NÃO deve avançar como se tivesse ligado.
        gateway.groupEngine.onNack(node, seq);
    } else {
        gateway.tracker.onAck(node, seq);
        // Fase 7a: só o ACK OK avança o handshake do motor de grupos.
        gateway.groupEngine.onAck(node, seq);
    }
}

void IrrigationModule::handleGwAck(const meshtastic_MeshPacket &mp, const Header &h)
{
    Ack ack;
    if (!decodeAck(mp.decoded.payload.bytes, mp.decoded.payload.size, ack)) {
        LOG_WARN("Irrigation GW: bad ACK payload from 0x%08x", mp.from);
        return;
    }
    confirmCommand(mp.from, ack.ackedSeq, ack.reason, ack.status == ACK_OK);
    // Reconciliação de epoch (mesma regra do HB — decisão §3).
    gwReconcileEpoch(mp.from, ack.configEpoch);
}
```

- [ ] **Step 3: Verificar build nativo**

Run: `pio run -e native`
Expected: SUCCESS.

- [ ] **Step 4: Rodar a suíte completa (garantir não-regressão)**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN) — nenhuma regressão no cmdtracker/grupos/monitor.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "refactor(irrigation): extrai confirmCommand de handleGwAck (sem mudança de comportamento)"
```

---

### Task 4: Drive local em `gwSendValveCmd` + ACK sintético no `gwTick`

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (assinatura de `gwSendValveCmd` c/ origem default; membro `pendingLocalAck`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp:2366-2393` (branch local em `gwSendValveCmd`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp:3211` (`gwTick` drena a fila)

**Interfaces:**
- Consumes: `isLocalTarget` (Task 1), `confirmCommand` (Task 3), `nodeDB->getNodeNum()`, `valves`/`gpos` (membros existentes).
- Produces: `gwSendValveCmd(..., AuditOrigin origin = AuditOrigin::PAINEL)` — passa a acionar saída local quando o alvo é o próprio nó. Enfileira ACK sintético.

- [ ] **Step 1: Assinatura + membro da fila em `IrrigationModule.h`**

Alterar a declaração de `gwSendValveCmd` (adicionar parâmetro default no fim):

```cpp
    uint32_t gwSendValveCmd(uint32_t node, uint8_t index, uint8_t tipo, uint8_t action, uint16_t durationS,
                            uint8_t zoneId, uint8_t attempts,
                            AuditOrigin origin = AuditOrigin::PAINEL); // origin usado só no drive local
```

Adicionar, na seção de membros privados (perto de `txSeq`):

```cpp
    // ACK sintético do drive local (Task motor): confirmado no próximo gwTick para não
    // reentrar no loop de emissão do motor de grupos (noteSent roda depois de gwSendValveCmd).
    struct LocalAck { uint32_t node = 0; uint32_t seq = 0; };
    LocalAck pendingLocalAck[8];
    uint8_t pendingLocalAckCount = 0;
```

- [ ] **Step 2: Branch local no topo de `gwSendValveCmd`**

Em `IrrigationModule.cpp`, no início de `gwSendValveCmd` (antes de `allocDataPacket`), inserir:

```cpp
    // Alvo = próprio gateway ⇒ aciona a saída LOCAL diretamente (sem rádio).
    if (isLocalTarget(node, nodeDB->getNodeNum())) {
        // Modo seguro: bloqueia ativação (paridade com a estação); desligar segue permitido.
        if (safeMode && action == 1) {
            auditEvent(origin, tipo == 1 ? AuditAction::GPO_ON : AuditAction::ABRIR, index, AuditResult::NACK, node);
            return 0;
        }
        if (tipo == 1) {
            gpos.command(index, action, durationS, millis());
        } else if (action) {
            valves.open(index, durationS, 0 /*teto compilado; clamp de maxMin já no chamador*/, millis());
        } else {
            valves.close(index);
        }
        auditEvent(origin,
                   action ? (tipo == 1 ? AuditAction::GPO_ON : AuditAction::ABRIR)
                          : (tipo == 1 ? AuditAction::GPO_OFF : AuditAction::FECHAR),
                   index, AuditResult::OK, node);
        uint32_t usedSeq = ++txSeq;
        // Enfileira ACK sintético (drenado no gwTick, após noteSent do motor de grupos).
        if (pendingLocalAckCount < 8) {
            pendingLocalAck[pendingLocalAckCount].node = node;
            pendingLocalAck[pendingLocalAckCount].seq = usedSeq;
            pendingLocalAckCount++;
        }
        LOG_DEBUG("Irrigation GW: drive LOCAL zone=%u idx=%u tipo=%u action=%u dur=%u", zoneId, index, tipo, action, durationS);
        return usedSeq;
    }
```

- [ ] **Step 3: Drenar a fila no `gwTick`**

Em `IrrigationModule.cpp`, dentro de `gwTick()` (início do corpo, antes do processamento do scheduler), inserir:

```cpp
    // Drena ACKs sintéticos do drive local: confirma DEPOIS que o loop de emissão
    // do motor de grupos registrou noteSent (evita reentrância).
    for (uint8_t i = 0; i < pendingLocalAckCount; i++)
        confirmCommand(pendingLocalAck[i].node, pendingLocalAck[i].seq, 0 /*reason*/, true /*ok*/);
    pendingLocalAckCount = 0;
```

- [ ] **Step 4: Passar origem correta nos dois emit sites relevantes**

No emit do **scheduler direto** (`IrrigationModule.cpp:3097-3101`, o `gwSendValveCmd(z->node, ...)` do ramo OPEN do scheduler), acrescentar o argumento de origem:

```cpp
            gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts, AuditOrigin::CRONOGRAMA);
```

No emit do **motor de grupos** (`IrrigationModule.cpp:3377`, `gwSendValveCmd(em.node, ...)`), acrescentar:

```cpp
            uint32_t seq = gwSendValveCmd(em.node, em.index, em.tipo, em.action, em.durationS, em.zoneId, attempts, AuditOrigin::GRUPO_HIDRAULICO);
```

> Demais chamadores mantêm o default `PAINEL` (manual/portal). O parâmetro só afeta o registro de auditoria do drive local.

- [ ] **Step 5: Verificar build nativo**

Run: `pio run -e native`
Expected: SUCCESS.

- [ ] **Step 6: Rodar a suíte completa (não-regressão)**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN).

- [ ] **Step 7: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): gateway aciona saída local (zona node==self) com ACK sintético"
```

---

### Task 5: Painel — "Gateway (local)" + rótulo "Motor / GPO" + mock

**Files:**
- Modify: `data/irrigacao/app.js:597-601` (`renderZones` busca `/overview` p/ `selfNode`)
- Modify: `data/irrigacao/app.js:645,649` (passar `selfNode` a `zoneEditForm`)
- Modify: `data/irrigacao/app.js:669-700` (`zoneEditForm` injeta chip "Gateway (local)")
- Modify: `data/irrigacao/app.js:745-746` (rótulo "Motor / GPO")
- Modify: `data/irrigacao/mock.js` (campo `selfNode` no `/overview` + zona-motor local exemplo)

**Interfaces:**
- Consumes: `/api/irrigation/overview` → `selfNode` (Task 2).

CI/banca-only (frontend não é native-testável). Verificação = preview com `mock.js`.

- [ ] **Step 1: `renderZones` obtém `selfNode` e o repassa**

Em `data/irrigacao/app.js`, `renderZones` (linha ~597). Trocar o `Promise.all` para também buscar overview e derivar `selfNode`:

```js
async function renderZones() {
  const [rows, stations, ov] = await Promise.all([
    getJson('/zones'),
    getJson('/stations').catch(() => []),
    getJson('/overview').catch(() => ({})),
  ]);
  const selfNode = num(ov && ov.selfNode) || 0;
```

E nas duas chamadas de `zoneEditForm` (linhas ~645 e ~649), passar `selfNode`:

```js
  view.querySelector('[data-znew]').addEventListener('click', () => zoneEditForm(null, rows, stations, selfNode));
```
```js
      zoneEditForm(z || null, rows, stations, selfNode);
```

> Confirmar que as demais linhas do `Promise.all`/`renderZones` que usavam `[rows, stations]` continuam válidas (agora há um 3º elemento `ov`).

- [ ] **Step 2: `zoneEditForm` recebe `selfNode` e injeta o chip "Gateway (local)"**

Assinatura (linha ~669):

```js
function zoneEditForm(z, rows, stations, selfNode) {
```

Logo após `const sts = Array.isArray(stations) ? stations : [];` (linha ~682), montar a lista de alvos incluindo o gateway local no topo:

```js
  const targets = selfNode
    ? [{ node: selfNode, name: 'Gateway (local)' }, ...sts]
    : sts.slice();
```

Trocar o bloco `stationChips` (linhas ~691-700) para iterar `targets` em vez de `sts`:

```js
    const stationChips = targets.length
      ? targets
          .map((s) => {
            s = s || {};
            const active = num(s.node) === st.node ? ' active' : '';
            const label = s.name ? esc(s.name) : nodeHex(s.node);
            return `<button class="pill${active}" data-node="${num(s.node)}">${label}</button>`;
          })
          .join('')
      : '<div class="empty">Nenhuma estação — pareie uma antes.</div>';
```

> O default de `st.node` para nova zona (linha ~675) continua sendo a 1ª estação; o operador escolhe "Gateway (local)" pelo chip quando quiser saída local.

- [ ] **Step 3: Rótulo do tipo GPO → "Motor / GPO"**

Linha ~746, trocar o texto do botão `data-tipo="1"`:

```js
             <button class="seg wide${st.tipo === 1 ? ' active' : ''}" data-tipo="1">Motor / GPO</button>
```

- [ ] **Step 4: `mock.js` — `selfNode` no overview + zona-motor local**

Em `data/irrigacao/mock.js`, no objeto de resposta de `/overview`, adicionar `selfNode` (usar um valor distinto das estações mockadas, ex.: `0x0A0A0A0A`):

```js
    selfNode: 168430090, // 0x0A0A0A0A — "Gateway (local)"
```

E na lista mock de `/zones`, adicionar uma zona-motor local:

```js
    { id: 9, name: 'Motor terreno', node: 168430090, tipo: 1, index: 0, maxMin: 120, padraoMin: 30, fonteInput: -1 },
```

- [ ] **Step 5: Preview manual (offline)**

Descomentar `<script src="mock.js">` em `data/irrigacao/index.html`, abrir o painel, ir em **Zonas › Nova zona**: confirmar que o chip **"Gateway (local)"** aparece no topo, que o tipo mostra **"Motor / GPO"**, e que a zona "Motor terreno" lista com alvo Gateway (local). Recomentar o include ao terminar.

- [ ] **Step 6: Commit**

```bash
git add data/irrigacao/app.js data/irrigacao/mock.js
git commit -m "feat(irrigation): painel oferece 'Gateway (local)' e rótulo Motor/GPO no form de zona"
```

---

## Self-Review (preenchido)

**Cobertura do spec:**
- Caso 1 (drive local) → Task 1 (predicado) + Task 4 (branch local + ACK sintético) + Task 3 (confirmCommand).
- Caso 2 (motor remoto) → já funciona; entrega de UX = Task 5 (rótulo "Motor / GPO"); acionamento remoto inalterado (default do param).
- selfNode p/ o form → Task 2.
- Fail-safe local → já coberto pelo tick global do `runOnce` (spec §Diagnóstico); nenhum task novo necessário.
- safeMode bloqueia ativação local → Task 4 Step 2.
- Auditoria do drive local → Task 4 (origem via param; CRONOGRAMA/GRUPO_HIDRAULICO nos emit sites).

**Placeholders:** nenhum — todo passo tem código/comando concreto.

**Consistência de tipos:** `isLocalTarget(uint32_t,uint32_t)` (Task 1) usado igual na Task 4. `confirmCommand(uint32_t,uint32_t,uint8_t,bool)` declarado na Task 3 e chamado na Task 3 (handleGwAck) e Task 4 (gwTick) com a mesma assinatura. `gwSelfNode()` (Task 2) consumido pelo front via `/overview.selfNode` (Task 5). `AuditOrigin` enum: valores reais `PAINEL`/`CRONOGRAMA`/`GRUPO_HIDRAULICO` (verificados em `AuditLog.h`).

**Risco conhecido:** reconcile de grupo por HB não cobre saída local (sem HB de si mesmo) — aceito no spec; ACK sintético + fail-safe local cobrem o handshake e a segurança.
