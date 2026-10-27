# Fonte de Acionamento unificada (Modo Remoto + Espelho)

> Data: 2026-10-27 · Branch: `sistema-irrigacao` · Projeto greenfield (sem migração de blobs legados)

## Problema

Hoje há dois caminhos separados que fazem "entrada digital vira comando de
saída", com comportamentos **hardcoded**:

- **Modo Remoto** (`RemoteButtonEdge.h` + `RemoteButtonTable` +
  `RemoteFallbackTracker` + `RemoteLedFsm`): detecção só de **borda de subida**
  (debounce fixo 50 ms, rearma ao soltar), ação **TOGGLE fixa**. Botoeira →
  `RemoteTrigger{inputIdx}` → gateway resolve zona-alvo pela tabela → `TOGGLE`.
  Fallback P2P direto se o gateway não confirmar em `remoteFallbackMs`.
  Feedback por **LED dedicado** em `pinsRemoteLed[2]`.
- **Modo Espelho** (`MirrorMode.cpp`), só no gateway: detecção por **nível**
  (debounce fixo 100 ms), ação **segue nível** (sobe=OPEN, desce=CLOSE),
  renovação de OPEN a cada 60 s (fail-safe `OPEN_S=120`). Zona ligada via
  `Zone::fonteInput`/`fonteEnabled`; bypassa o scheduler enquanto a entrada ativa.

Limitações que motivam o refactor:

1. **Detecção de borda e estado lógico não são configuráveis.** Remoto é
   sempre borda-subida→toggle; espelho é sempre nível→segue. Nenhuma escolha.
2. **Feedback exige pino de LED dedicado** (`pinsRemoteLed`). Boards reais não
   têm GPIO sobrando → a botoeira fica sem feedback físico.
3. **Dois code paths quase idênticos** (mesma espinha: fonte → intenção
   ABRIR/FECHAR → renovação 60 s → fail-safe → despacha zona), com duplicação
   de debounce, renovação e teto de abertura.

## Objetivo

Unificar Remoto + Espelho numa única **Fonte de Acionamento**: uma entrada
digital, com **política configurável** por entrada, que emite intenção lógica
(`ABRIR`/`FECHAR`/`TOGGLE`) a uma zona-alvo (local ou via mesh). Feedback de
comando passa a usar o **LED de status de placa existente** (`LedPatternController`),
sem pino dedicado.

Escopo: **somente** os dois modos de entrada digital. Controle de Nível
(sensor remoto com histerese) e Interlock (segurança) ficam fora — são primos,
mas de escopo maior; ficam para spec futura.

## Decisões-chave

### 1. A política mora no nó que tem a entrada física

Descoberta que define a arquitetura: o gateway **não enxerga o nível contínuo**
de uma entrada remota — só recebe o evento (`RemoteTrigger`). O espelho só
funciona hoje porque lê GPIO **local** do gateway. Logo, "seguir nível" de uma
entrada remota é impossível sem a fonte informar a intenção.

Portanto: **o nó-fonte avalia sua própria política** (`{polaridade,
nível/borda, ação}`) e emite **intenção lógica** — não o nível cru. O gateway
vira **despachante único**.

- **1 avaliador** — `InputActuationPolicy` (classe pura, testável), uma
  instância por entrada digital, roda em estação OU gateway.
- **1 despachante** — `gwFireRemote` generalizado: aceita **ação explícita**
  (`ABRIR`/`FECHAR`/`TOGGLE`) em vez de sempre toglar. Resolve TOGGLE contra o
  estado atual da zona (`gwZoneIsOpen`), como já faz.
- **Loopback local** — quando a entrada está no próprio gateway, a intenção vai
  direto ao despachante sem hop de mesh (comportamento que `gwFireRemote` já
  tem para o nó local).

### 2. Enum de política por entrada digital

```
enum class InputMode : uint8_t {
    DISABLED = 0,   // entrada não é fonte de acionamento
    FOLLOW,         // nível: ativo→ABRIR, inativo→FECHAR (+ re-afirma a cada 60 s)
    EDGE_BOTH,      // borda: sobe→ABRIR, desce→FECHAR (determinístico)
    EDGE_TOGGLE,    // borda de subida: TOGGLE (desencorajado sem feedback)
    EDGE_OPEN,      // borda de subida: sempre ABRIR (desencorajado)
    EDGE_CLOSE,     // borda de subida: sempre FECHAR (desencorajado)
};
```

- **Polaridade / optocoupler**: reaproveita o bit existente `digitalInActiveLow`.
  Inverte "ativo" → em `EDGE_BOTH`, sobe↔desce trocam de graça; em `FOLLOW`,
  ativo↔inativo trocam. Nenhuma config nova para inversão.
- **Pulso-vs-estado no alvo**: derivado do `tipo` da zona (0=válvula pulsada
  via H-bridge, 1=GPO nível). Já tratado target-side em `gwSendValveCmd`. A
  fonte **não** decide isso; só emite ABRIR/FECHAR.
- **Modos recomendados**: `FOLLOW` e `EDGE_BOTH` são determinísticos — a
  **posição da chave é a intenção**, dispensam feedback. `EDGE_TOGGLE`/
  `EDGE_OPEN`/`EDGE_CLOSE` existem por versatilidade, mas são documentados como
  desencorajados em board sem feedback (estado ambíguo).

### 3. Renovação / fail-safe na fonte

`FOLLOW` com entrada ativa **re-afirma ABRIR a cada 60 s** (migra o
`RENEW_MS=60000` do `MirrorMode` para o avaliador da fonte). A válvula latch
fecha sozinha no teto compilado se as renovações pararem (fonte morreu / enlace
caiu) → fail-safe preservado. `EDGE_BOTH`/`EDGE_*` são eventos; não renovam.

### 4. Protocolo

`RemoteTrigger{inputIdx}` → `RemoteTrigger{inputIdx, action}`, onde
`action ∈ {TOGGLE, OPEN, CLOSE}`. Greenfield: `action` é **sempre explícito**
(sem default-TOGGLE de compatibilidade). A estação resolve a política localmente
e manda a intenção já decidida.

### 5. Feedback no LED de placa (sem pino dedicado)

O `LedPatternController` já é uma máquina de **padrões de pisca priorizada** de
**um** LED onboard (`pinLed` / `LED_POWER`), com modos:

```
PAIRING > BATTERY_SOS > OUTPUT_OPEN > CONFIG_PENDING > NO_GATEWAY > NORMAL
```

Dobra-se o feedback de comando **dentro dele**, como padrões transientes:

| Modo novo | Padrão | Dispara em |
|---|---|---|
| `CMD_INFLIGHT` | pisca rápido | entre o aperto e o `RemoteLed`/ACK voltar |
| `CMD_OK` | 2 pulsos curtos → decai | zona confirmou (estado real via `RemoteLed`/ACK) |
| `CMD_FAIL` | rajada curta tipo SOS | timeout/NACK (nem gateway nem fallback P2P confirmaram) |

- **Prioridade**: `CMD_*` fica **abaixo** de segurança (`PAIRING`/`BATTERY_SOS`)
  e **acima** de `NORMAL`/`OUTPUT_OPEN`/`NO_GATEWAY` **só na janela transiente**
  (~3–5 s), depois decai para o modo de status normal.
- **Timing transiente** mora no chamador (`refreshLedMode` guarda um
  `feedbackUntilMs`); `LedPatternController::ledOn` continua função pura do tempo.
- As **transições continuam vindo do que já existe**: `RemoteLed` (carrega o
  estado **real** da zona, não um mero "recebi"), `RemoteFallbackTracker`,
  `_pendingDirect`/ACK. Só mudam de superfície de saída: renderizam no LED de
  status em vez de um pino dedicado.
- **Nó só-botoeira** (sem válvula local): `OUTPUT_OPEN` nunca dispara → o LED
  fica praticamente livre para o feedback de comando. Encaixe natural.

## Estrutura de código

### Removido (greenfield)

- `MirrorMode.cpp` / `MirrorMode.h` (absorvido por `InputActuationPolicy` +
  despachante).
- `RemoteButtonEdge.h`: `RemoteButtonEdge` (vira caso do avaliador unificado).
  `compileFallbackRoute`/`remoteToggleAction` permanecem (realocados).
- `RemoteLedFsm.h` e os campos `pinsRemoteLed[2]`, `digitalInLedIdx`, o uso do
  pacote `RemoteLed` como driver de pino dedicado.
- `Zone::fonteInput` / `Zone::fonteEnabled` (bind do espelho) — o alvo passa a
  ser referenciado só pela `ActuationSource`.

### Novo / alterado

- **`InputActuationPolicy`** (classe pura): entrada `(raw, nowMs)` + config
  `{InputMode, activeLow}`; saída: `Intent { NONE, OPEN, CLOSE, TOGGLE }`.
  Encapsula debounce (configurável, default 50/100 ms conforme modo), detecção
  de borda/nível, e renovação de 60 s para `FOLLOW`. Substitui tanto
  `RemoteButtonEdge::update` quanto `MirrorMode::update`.
- **`ActuationSource` / `ActuationSourceTable`**: generalização do
  `RemoteAssoc`/`RemoteButtonTable`. Mantém a estrutura "zona-alvo + lista de
  gatilhos (nó, inputIdx)". Ganha o `InputMode` por gatilho/entrada.
  - *Nomes de código*: avaliar na implementação se renomeia símbolos ou mantém
    `RemoteButtonTable`/`RemoteAssoc` e usa "Fonte de Acionamento" só na
    UI/docs. Decisão adiável; não bloqueia a arquitetura.
- **`gwFireRemote(node, inputIdx, action)`**: assinatura ganha `action`
  explícito. `remoteToggleAction` só é chamado quando `action == TOGGLE`.
- **`LedPatternController::Mode`**: + `CMD_INFLIGHT`, `CMD_OK`, `CMD_FAIL`.
  `ledOn` ganha os padrões. `refreshLedMode` ganha a janela transiente de
  feedback com prioridade.
- **`IrrigationSettings`**: redefinido limpo (versão 1 do esquema novo, sem
  cadeia de migração v1→v8). Campo novo `digitalInMode[MAX_DIGITAL_IN]` (ou
  packed 3 bits × 4). Remove `pinsRemoteLed`, `digitalInLedIdx`.
- **`IrrigationProto::RemoteTrigger`**: + campo `action`.

## Fluxo de dados (depois)

```
[nó-fonte]  GPIO → InputActuationPolicy(config) → Intent(OPEN/CLOSE/TOGGLE)
                                                      │
                  local? ──sim──→ gwFireRemote(local, idx, action)
                    │no
                    └→ RemoteTrigger{idx, action} ──mesh──→ [gateway]
                                                               │
[gateway]  handleRemoteTrigger → gwFireRemote(from, idx, action)
             → resolve zona-alvo (ActuationSourceTable.findByTrigger)
             → ABRIR/FECHAR (TOGGLE resolve vs gwZoneIsOpen)
             → gwSendValveCmd (pulso p/ válvula, nível p/ GPO) + retries
             → gwPushRemoteLed (estado real) ──mesh──→ [nó-fonte]
                                                          │
[nó-fonte]  handleRemoteLed → LedPatternController CMD_OK/CMD_FAIL (transiente)
             + fallback P2P se nenhum RemoteLed em remoteFallbackMs (ACK → CMD_*)
```

## Tratamento de erro (preservado)

- Comando gateway→estação mantém retries (`CommandTracker`, default 3 tentativas
  por `StationEntry`) → `CMD_FAIL` alert + `CMD_FAIL` no LED.
- Sem `RemoteLed` em `remoteFallbackMs` (default 5 s) → `RemoteFallbackTracker`
  dispara comando **P2P direto** à rota pré-compilada (`compileFallbackRoute`) →
  espera ACK (`_pendingDirect`) → LED `CMD_OK`/`CMD_FAIL`.
- `FOLLOW` sem renovação (fonte/enlace caiu) → válvula fecha no teto compilado.

## Testes

Suíte nativa (`./bin/run-tests.sh`). Alvos:

- **`InputActuationPolicy`** (novo `test_input_actuation_policy`): cada
  `InputMode` × polaridade; borda com debounce; `FOLLOW` com renovação de 60 s;
  rearme de `EDGE_TOGGLE`; transições `EDGE_BOTH`.
- **`gwFireRemote` com action** (estende `test_irrigation_*`): `TOGGLE` resolve
  vs estado; `OPEN`/`CLOSE` fixos; roteamento multi-gatilho.
- **LED** (estende `test_irrigation_ui` / padrão `LedPatternController`): janela
  transiente, prioridade `CMD_*` vs `OUTPUT_OPEN`/segurança, decaimento.
- **Migração dos testes existentes**: `test_irrigation_mirror` reescrito sobre
  `InputActuationPolicy` modo `FOLLOW`; `test_remote_button` / `test_remote_button_table`
  atualizados para o `action` explícito no protocolo.
- **Protocolo** (`test_irrigation_protocol`): encode/decode de `RemoteTrigger`
  com `action`.

## Specs relacionadas e contrato com a UI

A UI **não** está nesta spec. Vai numa spec própria —
`2026-10-27-irrigacao-fonte-de-acionamento-ui-design.md` — brainstormada em
separado. As duas ficam **amarradas** por:

- **Dependência de ordem**: firmware primeiro (define o contrato); UI depois
  consome. A UI não mergeia antes do núcleo estar verde.
- **Contrato compartilhado** (fonte da verdade = esta spec; a UI referencia):

  | Superfície | Antes | Depois (contrato novo) |
  |---|---|---|
  | Config por entrada | `digitalInBtnMask` bit + `digitalInActiveLow` bit | `digitalInMode[i]` (`InputMode`) + `digitalInActiveLow` bit |
  | LED | `pinsRemoteLed[2]`, `digitalInLedIdx` | **removidos** (feedback no LED de status) |
  | Tabela | `RemoteButtonTable` upsert + mapping de espelho (`gwApplyMirrorMapping`) | **tabela única** Fonte de Acionamento (espelho = preset `FOLLOW`) |
  | Protocolo exposto | `RemoteTrigger{inputIdx}` | `RemoteTrigger{inputIdx, action}` |

- **Endpoints afetados** (a UI-spec detalha o JSON): `IrrigationWebApi` /
  `ServicePortalApi` — campos `pinsRemoteLed`/`digitalInLedIdx` saem, entra
  seletor `InputMode` por entrada; tela de Espelho funde na de Fonte de
  Acionamento; `validateRemoteTriggers` estende para `action`.
- **Push de config de entrada pro nó dono** (requisito vindo da decisão de UI
  "híbrido B-como-grid"): a edição de `digitalInMode[i]` e `digitalInActiveLow`
  de uma estação é feita no grid do **gateway**, que **empurra** esses campos
  pro nó dono reusando o mecanismo de `configEpoch` já existente. A tabela de
  mapeamento `(nó,input)→zona` permanece gateway-side; `RemoteTrigger` continua
  `{inputIdx, action}` (modelo B). O nó mantém editor local mínimo como fallback.
- **Gateway expõe as entradas das estações** (requisito do grid agregado por nó
  da UI): o `GET /acionamento` lista entradas do gateway + por estação
  conhecida, logo o gateway precisa conhecer `digitalInMode`/polaridade das
  estações (reportado pela estação, ex.: anexo ao heartbeat/telemetria). Sem
  isso o grid só mostra as entradas locais do gateway.

Qualquer mudança no contrato acima exige editar **esta** spec e sincronizar a
UI-spec.

## Fora de escopo

- Controle de Nível (`LevelControlEngine`) e Interlock — unificação futura.
- Rename em massa de símbolos de código (decisão de implementação).
- Qualquer feedback acústico/buzzer (o LED de status resolve).
- UI / portal / endpoints JSON — spec própria (ver acima).
