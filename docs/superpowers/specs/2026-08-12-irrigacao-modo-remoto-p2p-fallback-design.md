# Modo Remoto P2P — fallback direto quando o gateway cai — design

**Branch:** `sistema-irrigacao`  ·  **Data:** 2026-08-12
**Fase:** modo remoto P2P (feature nova; estende o modo remoto gateway-central da spec
`2026-08-08-irrigacao-modo-remoto-design.md`)

## Objetivo

Hoje a botoeira → saída em outro nó **depende do gateway ativo**: todo comando passa por ele
(dono da tabela de associações, do estado autoritativo do toggle, do roteamento de zona,
fail-safe, intertravamentos e do push de LED). Gateway offline ⇒ a botoeira não aciona nada.

Este design adiciona um **fallback peer-to-peer**: quando o gateway está no ar, o caminho
gateway-central continua **inalterado**; quando o gateway não responde, a botoeira envia o
comando **direto ao nó alvo**, que inverte a própria saída. Resiliência sem perder a visão
central no caso normal.

## Decisões (confirmadas com o usuário)

1. **Fallback-only.** Gateway continua dono do estado/config/roteamento no caso normal. P2P
   direto só entra em ação quando o gateway não responde. Sem visão central perdida quando
   ele está no ar.
2. **Detecção por timeout do próprio gatilho.** Botoeira envia `REMOTE_TRIGGER` ao gateway e
   arma um timer `T_FALLBACK`. Sem `REMOTE_LED` dentro do prazo ⇒ reenvia como **comando
   direto** ao nó alvo. Sem rastreio de liveness do gateway; a 1ª pressão após a queda sofre o
   atraso do timeout.
3. **Escopo P2P = uma saída única num nó.** Fallback aciona **uma** saída direta (válvula/GPO)
   num nó alvo. **Sem grupo, sem intertravamento** — esses continuam só no gateway. O nó alvo
   aplica o **teto de duração compilado** (`MAX_OPEN_SECONDS`). Degradação simples e segura.
4. **Alvo direto auto-compilado pelo gateway** (aprovado — abordagem A). O gateway já conhece
   `zona → {node, valveId}`. Ao compilar o `SET_CONFIG` da botoeira, deriva a rota de fallback
   `{targetNode, outputKind, outputId}` **quando a zona é de nó único**. Zona = grupo multi-nó
   ⇒ **sem fallback** compilado (LED expira p/ apagado; bate com "só saída única").
5. **Toggle resolvido no nó alvo.** Comando direto carrega **ação toggle**; o alvo inverte a
   **própria** saída (ele já é dono desse estado). Sem estado central. Se o gateway togglou a
   saída pouco antes de cair, a inversão parte do estado **real do alvo** — sem desync.
6. **`T_FALLBACK` default ~1.5–2 s** (poucos airtimes de `REMOTE_TRIGGER`), ajustável em config.

## Diagnóstico do estado atual (verificado no código)

- **Modo remoto gateway-central** já existe (spec `2026-08-08`): `MSG_REMOTE_TRIGGER=14`
  (estação→gateway, fire-and-forget), `MSG_REMOTE_LED=15` (gateway→estação, change-driven),
  `RemoteButtonTable` (associações **só no gateway**), `RemoteLedFsm` (FSM de LED
  IDLE→BLINK→SOLID→OFF), `RemoteButtonEdge` (borda+debounce).
- **Comando de saída** `CmdValvula{valveId,action,durationS}` e `CmdGpo{gpoId,action,durationS}`
  (`IrrigationProtocol.h:73-83`). `action` hoje = `0` fechar / `1` abrir. **Livre: `2`.**
- **Auth = posse-da-PSK** (`senderAuthorizedBy`, `IrrigationProtocol.h:18-23`) + anti-replay
  por remetente (`seqTable`). Já permite **nó→nó** direto: qualquer nó da mesma PSK é
  autorizado. O antigo "quebra auth" (Fora de escopo da spec base) **não** bloqueia um peer da
  mesma PSK.
- **Nó alvo é dono da própria saída**: `GpoController`/válvula aplicam estado + teto de duração
  compilados localmente. Toggle local = ler `isOn(id)` e inverter.
- **`SET_CONFIG`** (epoch-bumped, §5.4) já é o caminho que empurra config de pinos por estação.
  Rota de fallback entra como campos novos nesse blob.
- **ABI settings** hoje **v7 (184 B)** após a spec base (`pinsRemoteLed`/`digitalInBtnMask`/
  `digitalInLedIdx`). Bumps aditivos ao fim.

## Arquitetura da mudança

### Fluxo — gateway NO AR (inalterado)

`botoeira → REMOTE_TRIGGER → gateway (toggle + gwSendValveCmd + fail-safe/interlock/grupo)
→ nó alvo → ACK → gateway → REMOTE_LED → botoeira`.

### Fluxo — gateway OFFLINE (novo)

1. **Botoeira**: borda debounced → LED **pisca** + envia `REMOTE_TRIGGER{i}` ao gateway +
   arma `pendingTrigger{inputIdx, dueMs = now + T_FALLBACK}`.
2. Chegou `REMOTE_LED` antes de `dueMs` ⇒ caminho normal; limpa pending. (Gateway no ar.)
3. `dueMs` expirou sem `REMOTE_LED` **e** existe rota de fallback compilada p/ a entrada *i* ⇒
   envia **direto** ao `targetNode`: `CmdValvula`/`CmdGpo` com **`action = ACTION_TOGGLE`**
   (`outputId` = `outputId` compilado), seq próprio + PSK. Rearma um timer curto de ACK direto.
4. **Nó alvo**: decodifica; auth PSK + anti-replay; `action==ACTION_TOGGLE` ⇒ inverte a saída
   local (`isOn(id)` ? desligar : ligar com teto `MAX_OPEN_SECONDS`); **ACK direto à botoeira**.
5. **Botoeira**: ACK direto OK ⇒ LED **fixo**; NACK/timeout do ACK direto ⇒ LED **apaga**.
   Reusa `RemoteLedFsm` (o ACK direto dirige `SOLID`/`OFF` no lugar do `REMOTE_LED` push).
- **Sem rota compilada** (zona = grupo): nada a fazer no fallback; LED expira p/ apagado.

### Núcleo da estação (nó com botoeira)

- **Estado pendente por entrada-botão**: `pendingTrigger[inputIdx] = { armed, dueMs }`. Uma
  unidade pura **`RemoteFallbackTracker`** (native-tested): `arm(inputIdx, nowMs)`,
  `onRemoteLed(inputIdx)` limpa, `tick(nowMs)` → devolve entradas cujo `dueMs` expirou e que
  têm rota de fallback ⇒ o módulo emite o comando direto. Sem retry além do 1º direto
  (fire-and-forget, humano repressiona).
- **Rota de fallback por entrada-botão**: derivada da config (campos ABI novos), lida no boot
  e a cada `SET_CONFIG`. `outputKind` (válvula/GPO) + `outputId` + `targetNode`. `targetNode==0`
  ⇒ sem fallback.
- **RemoteLedFsm**: passa a aceitar **duas** fontes de confirmação — `REMOTE_LED` (push do
  gateway) **ou** ACK direto do alvo. Ambas levam a `SOLID`; NACK/timeout → `OFF`.

### Núcleo do nó alvo (saída)

- Handler de `CmdValvula`/`CmdGpo` passa a aceitar `action==ACTION_TOGGLE`: lê estado atual da
  saída e comanda o inverso (ligar herda o teto `MAX_OPEN_SECONDS`; `durationS` ignorado no
  toggle-on ⇒ biestável com teto). ACK reflete o novo estado (`valveStates`/`gpoStates`).
- Nenhum caminho novo de rede: reusa o roteamento e o ACK já existentes de comando de saída.

### Núcleo do gateway

- **Compilação da rota de fallback**: ao montar o `SET_CONFIG` de cada estação com botoeira,
  para cada associação cujo gatilho está naquela estação, se a `targetZone` é de **nó único**
  (`Zone.node != 0` e não é grupo multi-nó), emite `{targetNode = Zone.node, outputKind,
  outputId = Zone.valveId}` nos campos ABI novos da entrada-botão correspondente. Zona = grupo
  ⇒ campos zerados (sem fallback). Unidade pura **`compileFallbackRoute(assoc, zone)`**
  (native-tested).
- **Reconciliação**: nenhuma lógica nova. O gateway já reaprende o estado real das saídas pelos
  heartbeats; um toggle feito em P2P é absorvido no próximo heartbeat. Janela de divergência
  enquanto o gateway está fora = **aceita**.

## Protocolo (aditivo; VERSION continua 1)

- **`ACTION_TOGGLE = 2`** para `CmdValvula`/`CmdGpo`. Semântica: alvo inverte a própria saída.
  `0`/`1` inalterados; nós de firmware antigo nunca recebem `2` (só o gateway/botoeira novos
  emitem). Codec já serializa `action` como `uint8` — **sem mudança de wire**, só de valor.
- **Sem tipos de mensagem novos.** Comando direto = `MSG_CMD_VALVULA=1` / `MSG_CMD_GPO=2`
  existentes. Auth = `senderAuthorizedBy` + `seqTable.checkAndUpdate` (já nó→nó).
- `REMOTE_TRIGGER=14` / `REMOTE_LED=15` inalterados.
- **`APP_FW_VERSION` bump `0x0800`→`0x0900`** (esta geração) — usado pelo gateway p/ decidir
  se o nó alvo entende `ACTION_TOGGLE` antes de compilar a rota de fallback (ver não-regressão).

## ABI & persistência

- **Settings v7→v8** — apêndice no FIM (offsets v7 intactos), **por entrada-botão** (4).
  Ordem escolhida por alinhamento (struct alinha a 4; `uint16` em offset par; `sizeof` múltiplo
  de 4):
  - `uint32_t btnFallbackNode[4]` (offset 184; 0 = sem fallback).
  - `uint8_t btnFallbackOutId[4]` (offset 200; id local da saída no nó alvo).
  - `uint16_t remoteFallbackMs` (offset 204; T_FALLBACK; 0 ⇒ default compilado).
  - `uint8_t btnFallbackKind` (offset 206; 2 bits/entrada × 4: 0=válvula, 1=GPO, 3=nenhum).
  - `uint8_t pad3` (offset 207; padding explícito).
  - Total **184→208 B**. `version=8`. Migrar v1..v7→v8 (`migrateIrrigationSettings`),
    `IRRIGATION_SETTINGS_V7_SIZE=184`, atualizar `static_assert`s.
- **Ripple** (roteiro conhecido das fases anteriores): `StationEntry.blob` 184→208 ·
  `STATION_ENTRY` serialize +24 · `StationRegistry::SERIALIZED_MAX` · `gwBuildBackup` (emite os
  campos novos por estação) · `extractLight` tolera campos v8 extras (key-seek ignora chaves
  desconhecidas).
- **`RemoteButtonTable`** inalterada (associações só no gateway). A rota de fallback é
  **derivada** dela + zonas no momento do `SET_CONFIG`; não é armazenamento novo no gateway.

## UI

- **Nenhuma UI nova obrigatória.** A rota de fallback é auto-compilada; o operador não a digita.
- Opcional (fora do escopo desta fase): um indicador "fallback disponível" por associação no
  card do Modo Remoto (verde se a zona é de nó único; cinza se grupo). Deixado p/ depois.

## Fora de escopo

- **Grupo/intertravamento em P2P** — só no gateway (decisão: só saída única).
- **P2P como caminho normal** — descartado (fallback-only; visão central preservada no ar).
- **Renovação de fail-safe em P2P** — não; o alvo aplica um **teto compilado** único.
- **Rastreio de liveness do gateway** — não; detecção é por timeout do gatilho.
- **Retry além do 1º comando direto** — fire-and-forget (humano repressiona).
- **Bump de VERSION de protocolo / cifra** — nenhum; 100% aditivo (VERSION=1).

## Invariantes / não-regressão

- Protocolo VERSION=1. Caminho gateway-central **byte-idêntico** quando o gateway responde.
- `action` `0`/`1` intactos; `2` só emitido por gateway/botoeira novos.
- Nó alvo aplica teto de duração igual em comando de gateway e em toggle direto P2P.
- Modo seguro bloqueia ativação local igual (o handler de saída já checa antes de comandar).
- Nós de firmware antigo ignoram `action==2`? **Não** — precisam decodar; por isso o gateway só
  compila fallback para nós-alvo com firmware que entende `ACTION_TOGGLE`. **Restrição:** a rota
  de fallback só é compilada se o alvo reporta `APP_FW_VERSION >= 0x0900` (esta geração) no
  heartbeat/survey. Alvo antigo ⇒ sem fallback (degrada p/ gateway-only, seguro).

## Plano de testes

- **Nativo (unidade pura):**
  - `RemoteFallbackTracker` — arm, onRemoteLed limpa antes do timeout, tick expira e só emite
    quando há rota, sem retry duplo. (suíte nova)
  - `compileFallbackRoute` — zona nó-único ⇒ rota; zona grupo ⇒ vazia; alvo fw antigo ⇒ vazia.
    (anexo a webapi/gateway tables)
  - Toggle no alvo — `isOn` true ⇒ desliga; false ⇒ liga com teto. (anexo a `GpoController`/válvula)
  - `IrrigationProtocol` — `CmdValvula`/`CmdGpo` round-trip com `action==2`. (`test_irrigation_protocol`)
  - `migrateIrrigationSettings` v7→v8 + tolerância de `extractLight`. (suítes settings/backup)
- **Nativo (build):** `IrrigationModule.cpp` continua compilando/linkando (cola do módulo não é
  coberta por unidade — padrão das fases anteriores).
- **Banca (obrigatória — rádio não é nativo-testável):**
  - Gateway no ar: botoeira → saída no outro nó, LED pisca→fixo (caminho normal inalterado).
  - **Gateway desligado**: botoeira → após `T_FALLBACK`, comando direto → saída inverte no alvo
    → ACK direto → LED fixo. Nova pressão desliga. Alvo aplica teto de duração.
  - Zona = grupo, gateway desligado: botoeira pressiona → sem fallback → LED expira p/ apagado.
  - Gateway volta: heartbeat reconcilia o estado togglado em P2P.
- **CI:** build ESP32 obrigatório.
