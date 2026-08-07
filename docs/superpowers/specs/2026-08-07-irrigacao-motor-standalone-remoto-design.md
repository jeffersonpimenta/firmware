# Motor sem-válvula: gateway standalone + acionamento remoto — design

**Branch:** `sistema-irrigacao`  ·  **Data:** 2026-08-07
**Fase:** motor/bomba direta (segue meteorologia; reusa zonas/scheduler/grupos existentes)

## Objetivo

Permitir que um **motor** (saída a relé/contator = GPO) seja uma **zona de irrigação
de 1ª classe** que:

1. **Caso 1 — gateway standalone:** o gateway aciona um motor ligado à **sua própria
   placa** (saída local), irrigando **todo o terreno sem nenhuma válvula** e **sem
   depender de nenhuma estação remota**.
2. **Caso 2 — motor remoto:** o gateway aciona um motor ligado a **um nó/estação**
   remota.

Em ambos os casos o motor se comporta como **zona normal**: entra em **programas
semanais**, **execução manual** pelo painel, **supressão climática**,
**intertravamentos** e **teto fail-safe de duração**. Sem perder nenhuma
funcionalidade existente.

## Diagnóstico do estado atual (verificado no código)

- Drivers locais `valves` (`ValveController`) e `gpos` (`GpoController`) são
  configurados **incondicionalmente** no ctor do módulo
  (`IrrigationModule.cpp:249–251`) — o hardware de saída existe e está ativo mesmo
  no papel **GATEWAY**.
- O fail-safe `valves.tick()` / `gpos.tick()` roda no **topo do `runOnce()` para
  TODOS os papéis** (`IrrigationModule.cpp:898–904`) — inclusive gateway. Timers
  de saída local já auto-desligam. **Nada a mudar aqui.**
- MAS `gwSendValveCmd(node, index, tipo, action, dur, zoneId, attempts)`
  (`IrrigationModule.cpp:2366`) **sempre** faz `service->sendToMesh(...)`. Uma zona
  com `node` == próprio gateway vira um pacote de rádio para si mesmo, que o papel
  GATEWAY **não** processa como comando de estação → **nada aciona localmente**.
  **→ este é o gap real do Caso 1.**
- Zona `tipo=1` (gpo) **já é criável** (`parseZoneUpsert` aceita tipo 0/1 —
  `IrrigationWebApi.cpp:362`), **já aparece no form** do painel
  ("Válvula / GPO biestável" — `app.js:745`), **já é roteada** por scheduler +
  manual via `gwSendValveCmd(tipo=1)` → `CMD_GPO`, e a **estação já executa**
  (`handleCmdGpo` — `IrrigationModule.cpp:465` → `gpos.command`). **→ o Caso 2 já
  funciona** como zona GPO remota; falta só modelagem/rótulo de "motor" e oferecer
  "Gateway (local)" no mesmo form.
- Não há **teste nativo de unidade** que compile `IrrigationModule` (todas as
  suítes de irrigação testam unidades puras). `IrrigationModule.cpp` **compila e
  linka** no build nativo, mas a cola do módulo não é coberta por unidade — padrão
  das fases anteriores (cola = CI build + banca).

## Decisões (confirmadas com o usuário)

1. **Motor = zona `tipo=1` (GPO) reusada + rótulo.** Eletricamente motor =
   relé/contator = GPO. **Zero mudança de ABI** (`Zone.tipo` inalterado, `Zone.node`
   inalterado). Motor = zona `tipo=1` acionada **temporizada** (duração > 0);
   biestável (portão/luz/sirene) continua sendo a mesma zona via aba GPO com dur=0.
   Distinção é de **UI/uso**, não de tipo de dado.
2. **Alvo local sinalizado por `Zone.node == nodeDB->getNodeNum()`** (próprio nó do
   gateway) ⇒ acionamento local. Natural, sem sentinela nova nem ABI. O painel
   oferece **"Gateway (local)"** na lista de nós-alvo do form de zona.
3. **Ambos os casos** no escopo (motor na placa do gateway **e** numa estação).
4. Operação = **zona normal completa** (programas, manual, clima, intertravamento,
   fail-safe). Reusa a maquinaria existente de zona/scheduler/grupo.

## Arquitetura da mudança

### Núcleo — caminho de acionamento LOCAL (Caso 1)

Ponto único: **`gwSendValveCmd`**. Todo open/close de zona já passa por ele
(scheduler, manual, portal, mirror, grupo). Um só ponto a interceptar.

- **Decisão pura testável** — função livre em header:
  `bool isLocalTarget(uint32_t node, uint32_t selfNode)`
  = `node != 0 && node == selfNode`. Native-tested (suíte nova pequena ou anexo em
  `test_irrigation_gwtables`).
- **Em `gwSendValveCmd`**, no topo: se `isLocalTarget(node, nodeDB->getNodeNum())`:
  - `tipo==1` → `gpos.command(index, action, durationS, millis())`
  - `tipo==0` → `action` ? `valves.open(index, durationS, configMax, millis())`
    : `valves.close(index)` (`configMax` = 0 ⇒ usa teto compilado; o clamp de
    `maxMin` já é aplicado nos chamadores)
  - **safeMode**: se `action==1` e `safeMode`, **não aciona** (espelha estação).
  - **Auditoria**: `auditEvent(<origem>, ABRIR/FECHAR ou GPO_ON/GPO_OFF, index,
    OK)` — como a saída local não passa pelo handler de estação, é aqui que se
    registra. Origens reais do enum: `CRONOGRAMA` (scheduler), `PAINEL` (manual),
    `GRUPO_HIDRAULICO`, `NIVEL`, `CLIMA`. Como `gwSendValveCmd` hoje não recebe a
    origem, o **plano** decide: (a) adicionar um parâmetro `AuditOrigin` a
    `gwSendValveCmd` (todos os chamadores já sabem a origem), **ou** (b) auditar no
    chamador. Preferência: (a), default `PAINEL` para manter assinatura retro-compatível.
  - Ainda gera `usedSeq = ++txSeq` e chama `gateway.tracker.track(...)` (uniforme).
  - **Enfileira um ACK sintético** `{node=self, seq=usedSeq, ok=true}` numa fila
    curta (`pendingLocalAck[]`), **NÃO** confirma inline (evita reentrância dentro
    do loop de emissão do grupo, onde `noteSent` ainda não rodou).
  - Retorna `usedSeq` normalmente. **Não** chama `sendToMesh`.
- **Extrair `confirmCommand(uint32_t node, uint32_t seq, bool ok)`** do
  `handleGwAck` (`IrrigationModule.cpp:3603`): núcleo comum =
  `tracker.onAck` + (`ok` ? `groupEngine.onAck` : `groupEngine.onNack`) +
  alerta CMD_FAIL no NACK. `handleGwAck` passa a chamá-la; o dreno local também.
- **Em `gwTick`** (`IrrigationModule.cpp:3211`): **drenar** `pendingLocalAck[]`
  chamando `confirmCommand(node, seq, true)` — roda **depois** que o tick de
  emissão do grupo registrou `noteSent`, então o handshake do motor de grupos
  avança corretamente mesmo com bomba/válvula locais.

> **Reconciliação de grupo com saída local:** o reconcile de estado real em
> `handleGwHeartbeat` (`:3674+`) depende de heartbeat da estação — não há HB de si
> mesmo. Para o caso primário (motor solo, sem grupo) isso é irrelevante. Para um
> **grupo com bomba/válvula local**, o ACK sintético cobre o avanço do handshake;
> o reconcile por HB simplesmente não se aplica à saída local (ela é confiável e
> já tem fail-safe local por tick). Aceito para v1.

### Motor como zona (Casos 1 e 2) — UI + 1 campo de API

- **API**: expor `selfNode` (= `nodeDB->getNodeNum()`) no estado do gateway
  (overview/nodestate JSON) para o front oferecer "Gateway (local)".
- **Front `data/irrigacao/app.js`**:
  - Form de zona: a lista de nós-alvo ganha a opção **"Gateway (local)"**
    (value = `selfNode`), além das estações.
  - Rótulo do `tipo=1` no form passa a **"Motor / GPO"** (mesmo valor de dado).
  - `mock.js`: incluir `selfNode` e uma zona-motor local de exemplo.

### Standalone sem estações

- Zona local **não requer** `StationEntry`. Onde o código busca
  `stations.byNode(z->node)` para `retries` (ex.: `gwRunCommand:2743`), `null` ⇒
  `attempts` default (irrelevante no local, que confirma na hora).
- Scheduler/manual/grupos/clima/intertravamento já operam **por zona**; nenhum
  exige estação para uma zona local. Verificar na implementação que nenhum caminho
  aborta por ausência de `StationEntry`.

## Fora de escopo

- **Config de pino da estação remota** (qual GPIO é o motor no nó): já existe via
  **portal do nó (§7.2)** e **portal SERVICO (§11.8)**. Não se toca aqui.
- **Novo `tipo=2`**: descartado — motor reusa `tipo=1`.
- **Cifra / novo tipo de wire / bump de ABI**: nenhum. Mudança 100% aditiva.
- **Reconcile por HB de grupo com bomba local**: coberto por fail-safe local +
  ACK sintético; reconcile-por-HB não se aplica (sem HB de si mesmo).

## Invariantes / não-regressão

- `Zone` ABI (`IZN2`) intacta; protocolo VERSION=1; `IrrigationSettings` intacta.
- Zonas remotas (válvula e GPO) seguem exatamente o caminho de rádio atual — só
  `node==self` desvia para local.
- Fail-safe (teto de duração) vale igual em local e remoto (tick já roda p/ todos).
- Modo seguro bloqueia ativação local (paridade com estação).

## Plano de testes

- **Nativo (unidade pura):** `isLocalTarget` (self, zero, outro nó, node==0).
- **Nativo (build):** `IrrigationModule.cpp` deve continuar compilando/linkando.
- **Front:** `mock.js` renderiza zona-motor local; form oferece "Gateway (local)"
  e rótulo "Motor / GPO".
- **Banca:**
  - 1 nó (gateway standalone): criar zona-motor local → manual ON aciona relé da
    placa; auto-off no fim da duração; programa dispara e aciona; safeMode bloqueia.
  - 2 nós: zona-motor remota num nó → CMD_GPO liga o motor da estação; auto-off.
- **CI:** build ESP32 obrigatório (cola do módulo + endpoints não são nativo-testáveis).
