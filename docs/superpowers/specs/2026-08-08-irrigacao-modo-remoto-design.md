# Modo Remoto: botoeira(s) → saída em outro nó, com LED de feedback — design

**Branch:** `sistema-irrigacao`  ·  **Data:** 2026-08-08
**Fase:** modo remoto (feature nova; reusa zonas/roteamento/ACK/anti-replay existentes)
**Mockup de UI (fornecido pelo usuário):** `C:\Users\Jefferson\Downloads\Irrigacao Mobile.dc.html`
— aba **Mais → Modo Remoto** (lista L1153–1222, editor L1225–1313, view-model L2097–2217 e
L3355–3451). O mockup é a referência visual; os refinamentos abaixo (dedup de saída, sem pill
global, LED no nó da botoeira) **substituem** pontos específicos do mockup, por decisão do usuário.

## Objetivo

Uma **botoeira** (botão físico momentâneo) no gateway **ou** em qualquer nó aciona uma
**saída em outro nó ou no gateway**. Suporta **múltiplas botoeiras em múltiplos nós**
controlando **a mesma saída**. LEDs dão **feedback visual** do acionamento no nó da botoeira.

Casos de uso: botão junto a um equipamento liga um motor/bomba/válvula noutro ponto do
parque; vários operadores em pontos diferentes acionam a mesma saída; o LED confirma que o
comando saiu e que a saída ficou de fato ativa (inclusive quando outro nó a ativou).

## Diagnóstico do estado atual (verificado no código)

- **Entradas digitais** existem por nó: `IrrigationSettings::pinsDigitalIn[4]` +
  polaridade por bit `digitalInActiveLow` (`IrrigationSettings.h:43-44`). Hoje alimentam o
  **MirrorMode** no gateway e sensores de nível. **Botoeira = uma dessas entradas** marcada
  com papel "botão" (bit novo). O botão multifunção `pinBtn` **não** serve — já está todo
  usado (pareamento/portal/reset — `IrrigationModule.cpp:1655-1724`).
- **MirrorMode** (`MirrorMode.h`) já faz leitura de entradas digitais do gateway com
  **debounce (100 ms)** + polaridade e mapeia entrada→zona→comando
  (`IrrigationModule.cpp:3441-3476`). É *level-following*, só entradas locais do gateway, e
  mapeia para zonas do gateway. Modo remoto precisa de **borda de subida** (não nível) e de
  botoeiras **remotas** (num nó). Reaproveita-se o padrão de leitura/debounce, não a classe.
- **`MSG_REMOTE_CMD=12`** já existe: um nó não-gateway envia `RemoteCmd{zoneId,action,
  durationS}` ao gateway, que executa via `portalRunNetCommand` (roteia p/ o nó da zona,
  inclusive local). Autoridade = posse-da-PSK (`senderAuthorizedBy`) + anti-replay `seqTable`
  (`IrrigationModule.cpp:3620-3642`). **É a semente do modo remoto**, mas carrega `zoneId` —
  exige que o nó conheça o alvo. Modo remoto mantém o nó **burro** (só sabe "entrada i é
  botão") ⇒ mensagem nova de gatilho sem `zoneId`.
- **Roteamento de zona** `gwSendValveCmd`/`routeZoneToGroup` já trata alvo local + remoto,
  fail-safe (teto de duração), grupos e intertravamentos (fases anteriores). Alvo local =
  `Zone.node == nodeDB->getNodeNum()` (`isLocalTarget`, `GatewayTables.h`, fase motor-standalone).
- **LED**: um único `pinLed` + `LedPatternController` (modos de status). Modo remoto quer
  **LEDs dedicados** (sequência de pinos reservada, 2 por nó) → campos novos em settings.
- **ABI settings** hoje **v6 (180 B)** (`IrrigationSettings.h:113-115`). Bumps aditivos ao
  fim (fase 9 fez v5→v6). `StationEntry.blob` = 180, `STATION_ENTRY` serialize = 215.
- Tipos de mensagem vão até `MSG_CMD_MAINT=13` (`IrrigationProtocol.h:38`). Livres: 14, 15.

## Decisões (confirmadas com o usuário)

1. **Semântica = toggle-latch.** Cada borda de subida da botoeira **inverte** a saída
   alvo (liga↔desliga); permanece até nova pressão por **qualquer** botoeira associada. O
   gateway é dono do estado autoritativo e resolve o toggle. Fail-safe (teto de duração) é
   renovado periodicamente enquanto ligada, como o MirrorMode faz (`RENEW_MS`). **Sem campo
   de duração** por associação (bate com o mockup, que não tem duração e alterna estado).
2. **LED = no nó da BOTOEIRA + estado compartilhado** (supera o mockup, que punha o LED no
   nó da saída). Comportamento: **pisca** enquanto o comando está em trânsito → **acende
   fixo** quando a saída alvo é confirmada ligada (ACK/estado) → **apaga** em NACK/timeout.
   **Também acende** sempre que aquela saída estiver ativa por qualquer origem (outro nó,
   scheduler) — o gateway empurra o estado real **na mudança**.
3. **Arquitetura = gateway-central + 2 mensagens novas minúsculas.** Associações vivem no
   gateway (tabela nova + arquivo em flash, como zones/mirror/groups). Reusa auth PSK +
   anti-replay + ACK + roteamento de zona.
4. **Botoeira = entrada digital com papel "botão"** (reusa `pinsDigitalIn`, não pinos novos
   de botão). Papel exclusivo por entrada (botão ≠ sensor/mirror na mesma entrada).
5. **Gatilho fire-and-forget** (sem tracker/retry): botão é humano; pressão perdida → LED
   expira p/ apagado, operador repressiona.
6. **LEDs dedicados**: 2 pinos reservados por nó (`pinsRemoteLed[2]`), providos no
   provisionamento (hardware).

## Arquitetura da mudança

### Fluxo — botoeira em uma estação

1. **Estação**: borda de subida debounced na entrada-botão *i* → LED local **pisca** +
   envia `REMOTE_TRIGGER{inputIdx=i}` ao `boundGateway` (seq + auth PSK). Fire-and-forget.
2. **Gateway**: anti-replay (`seqTable.checkAndUpdate`) + `senderAuthorizedBy`; acha a(s)
   associação(ões) cujo gatilho = `(from, i)`; para cada, **toggla** a `targetZone`: se
   estado atual desligado → abrir (toggle-on), senão fechar. Aciona via `gwSendValveCmd`
   (herda fail-safe, intertravamento, coreografia de grupo, roteamento local+remoto).
3. **Nó alvo** dá ACK → gateway atualiza o estado conhecido da saída → empurra
   `REMOTE_LED{ledStates}` **na mudança** a todos os nós-gatilho daquela associação.
4. **Estação**: `REMOTE_LED` ligado → LED **fixo**; desligado → **apaga**. Sem push dentro
   do timeout → LED **apaga** (falha). Operador repressiona.
- **Botoeira local do gateway**: mesma busca **em processo**, zero rádio. Gateway aciona os
  próprios LEDs remotos localmente.

### Núcleo do gateway

- **`RemoteButtonTable`** (nova, `GatewayTables`-style; unidade pura native-tested):
  - Entrada: `{ id, enabled, targetZoneId, triggerCount, triggers[] }`; cada trigger =
    `{ node(uint32), inputIdx(uint8, 0..3), ledSlot(uint8: 0/1/255=nenhum) }`.
  - Cap: `MAX_REMOTE = 8` associações × `MAX_TRIGGERS = 4` gatilhos.
  - API: `upsert`/`remove`/`byId`/`findByTrigger(node,inputIdx)→lista`/`byTargetZone` (dedup)
    + `serialize`/`deserialize` (arquivo `/prefs/irrigation-remote.dat`, padrão save/load
    espelhando mirror/zones).
  - **Restrição** (bate com mockup): **um gatilho por nó por associação** — `upsert` de um
    trigger no mesmo nó substitui o anterior (lógica igual a `toggleRemoteDraftTrigger`).
- **Leitura de botoeiras locais do gateway**: novo detector de **borda + debounce** (unidade
  pura `RemoteButtonEdge`, native-tested; distinto do MirrorMode que é nível). Em `runOnce`,
  no bloco do gateway, lê `pinsDigitalIn` marcados como botão, com polaridade, e emite
  bordas → mesma busca `findByTrigger(self, i)`.
- **Resolução do toggle** (unidade pura): dado o estado atual conhecido da saída da
  `targetZone`, retorna ação (abrir/fechar). O estado vem do que o gateway já rastreia
  (`zoneOpen`/tracker/groupEngine).
- **Empurrão de LED (change-driven)**: quando o estado real de uma saída-alvo muda (ACK,
  heartbeat de reconcile, fechamento por fail-safe, comando de scheduler), o gateway
  recomputa os `ledStates` de cada nó-gatilho afetado e envia `REMOTE_LED` **só na mudança**
  (airtime limitado). Bitmap = estado por slot de LED do nó destinatário.

### Núcleo da estação (nó com botoeira)

- Precisa de conhecimento **local** para **piscar na hora** (antes do ida-e-volta): quais
  entradas são botão + qual LED cada uma pisca. **Derivado das associações e empurrado via
  `SET_CONFIG`** (bump de epoch §5.4) — mesmo caminho que a config de pinos da estação já
  percorre. Ver ABI abaixo (`digitalInBtnMask`, `digitalInLedIdx`, `pinsRemoteLed`).
- **Detector borda+debounce** por entrada-botão (mesma unidade pura `RemoteButtonEdge`) em
  `runOnce` (papel ESTAÇÃO): borda → envia `REMOTE_TRIGGER{i}` ao gateway + LED **pisca**.
- **Máquina de estados do LED** (unidade pura `RemoteLedFsm`, native-tested, estilo
  `SurveyBeacon`/`LedPatternController`):
  `IDLE →(press)→ BLINK →(REMOTE_LED on)→ SOLID | (REMOTE_LED off / timeout)→ OFF`.
  Uma instância por slot de LED (2 por nó). Botoeira local do gateway usa a mesma FSM.

### Protocolo (aditivo; VERSION continua 1; nós ignoram tipos desconhecidos)

- **`MSG_REMOTE_TRIGGER=14`** — corpo `{ uint8 inputIdx }`. Estação→gateway. Fire-and-forget.
  Auth = `senderAuthorizedBy(flags, from, boundGateway)` + `seqTable.checkAndUpdate`.
  Encoder/decoder em `IrrigationProtocol.{h,cpp}`; codec native-tested.
- **`MSG_REMOTE_LED=15`** — corpo `{ uint8 ledStates }` (bitmap: bit *s* = slot de LED *s*
  ligado). Gateway→nó, **change-driven**. É o sinal de confirmação (o push *é* o ACK do LED;
  sem ACK separado). Sem anti-replay estrito (estado idempotente; aplica o último recebido).

## UI — painel "Mais → Modo Remoto" (fiel ao mockup + refinamentos)

Base = mockup L1153–1313 / view-model L2097–2217 e L3355–3451. Frontend em `data/irrigacao/`
(`app.js`/`index.html`) reproduzindo a estrutura; endpoints CI-only.

**Refinamentos que substituem o mockup (decisão do usuário):**
1. **Sem pill "Ativo" global** — a mera presença de associação já habilita; mantém-se o
   toggle **"Habilitada/Desativada"** por associação.
2. **Card "Testar acionamento" com dedup por saída**: cada saída-alvo aparece **uma única
   vez**, mesmo com várias botoeiras (agrupar por `targetZoneId`) — mostra lista combinada de
   gatilhos + estado ao vivo + botão **Acionar** (toggle). (Mockup faz 1 linha por associação;
   trocamos por 1 linha por saída.)
3. **Editor**: seleção de nós+entradas de gatilho (um input por nó, vários nós — igual à
   lógica de mesma-nó-substitui do mockup) · saída alvo (zona; oferece **"Gateway (local)"**
   como os outros forms de zona) · **seletor de LED por gatilho** (LED 1 / LED 2 / nenhum no
   nó daquele gatilho) — **substitui** o seletor de LED-do-nó-da-saída do mockup, pois o LED
   agora vive no nó da botoeira · toggle habilitar.

**Endpoints CI-only** (`IrrigationWebEndpoints`, gated role==GATEWAY):
`GET /api/irrigation/remote` (lista+status ao vivo), `POST /api/irrigation/remote` (upsert),
`POST /api/irrigation/remote/delete`, `POST /api/irrigation/remote/command` (Acionar/toggle).
Helpers **puros** em `IrrigationWebApi` (native-tested em `test_irrigation_webapi`):
`parseRemoteUpsert`/`parseRemoteDelete`/`parseRemoteCommand`, `buildRemote`/`buildRemoteStatus`
(com **dedup por saída**), `validateRemoteTriggers`.

**Portal/SERVICO**: `pinsRemoteLed[2]` entram no editor de config field-complete já existente
(§11.8 / §7.2). Sem UI nova relevante além dos 2 campos de pino.

`mock.js`: estende com `/api/irrigation/remote` (lista/status/upsert/delete/command),
associações de exemplo (várias botoeiras → 1 saída), para preview offline do painel.

## ABI & persistência

- **Settings v6→v7** — apêndice no FIM (offsets v6 intactos):
  - `int8_t pinsRemoteLed[2]` (2 pinos de LED dedicados; -1 = ausente).
  - `uint8_t digitalInBtnMask` (bit *i* = entrada *i* é botoeira).
  - `uint8_t digitalInLedIdx` (2 bits por entrada × 4 = slot de LED 0/1/3=nenhum por botão).
  - Total **180→184 B**. `version=7`. Migrar v1..v6→v7 (`migrateIrrigationSettings`),
    `IRRIGATION_SETTINGS_V6_SIZE=180`, atualizar `static_assert`s.
- **Ripple** (idêntico ao que a fase 9 fez v5→v6, roteiro conhecido):
  `StationEntry.blob` 180→184 · `STATION_ENTRY` serialize 215→219 ·
  `StationRegistry::SERIALIZED_MAX` · `gwBuildBackup` (emite os campos novos por estação) ·
  `extractLight` tolera campos v7 extras (key-seek já ignora chaves desconhecidas).
- **`RemoteButtonTable`** = **só no gateway**, arquivo próprio `/prefs/irrigation-remote.dat`
  (não entra no blob de settings). Persistência espelha mirror/zones.
- **Backup §5.5**: incluir a `RemoteButtonTable` no envelope do gateway (como zonas/grupos)
  e no restore `importConfigTablesFromBackup` (seção `clients[0].config.remoteButtons`).

## Fora de escopo

- **Momentâneo (on-while-held)** e **duração por associação** — descartados (toggle-latch).
- **Peer-to-peer** (nó comanda nó direto) — descartado (quebra auth; sem visão central).
- **Reusar só `REMOTE_CMD`** sem mensagem nova — descartado (toggle exige estado que o nó
  não tem).
- **Cifra / bump de VERSION de protocolo** — nenhum; 100% aditivo (VERSION=1).
- **Retry do gatilho** — fire-and-forget por decisão.
- **Config de pino da saída remota** — já via portal §7.2 / SERVICO §11.8.

## Invariantes / não-regressão

- Protocolo VERSION=1; `Zone` (IZN2) intacta; MirrorMode intacto (entrada-botão é papel
  distinto de entrada-mirror).
- Uma entrada digital é **ou** mirror **ou** sensor **ou** botoeira (papel exclusivo).
- Fail-safe (teto de duração) vale igual em saída local e remota togglada.
- Modo seguro bloqueia ativação local (paridade com estação, via `gwSendValveCmd`).
- Nós de firmware antigo ignoram `REMOTE_TRIGGER`/`REMOTE_LED` e campos v7 (aditivo).

## Plano de testes

- **Nativo (unidade pura):**
  - `RemoteButtonTable` — upsert (incl. substituição mesmo-nó), remove, `byId`,
    `findByTrigger`, dedup `byTargetZone`, serialize/deserialize round-trip. (suíte nova)
  - `RemoteButtonEdge` — borda de subida + debounce, polaridade. (suíte nova ou anexo)
  - `RemoteLedFsm` — IDLE→BLINK→SOLID→OFF, timeout, aplicação de push. (suíte nova)
  - Resolução de toggle — estado→ação (abrir/fechar). (anexo)
  - `IrrigationProtocol` — codec `REMOTE_TRIGGER` + `REMOTE_LED` (encode/decode round-trip,
    buffer curto → 0). (`test_irrigation_protocol`)
  - `IrrigationWebApi` — `parseRemote*`/`buildRemote*` + **dedup por saída** +
    `validateRemoteTriggers`. (`test_irrigation_webapi`)
  - `migrateIrrigationSettings` v6→v7 + tolerância de `extractLight`. (suítes de settings/backup)
- **Nativo (build):** `IrrigationModule.cpp` deve continuar compilando/linkando (cola do
  módulo não é coberta por unidade — padrão das fases anteriores).
- **Front:** `mock.js` renderiza associações (várias botoeiras → 1 saída, dedup no card);
  editor oferece nós/entradas, saída (com "Gateway (local)"), LED por gatilho; `node --check`.
- **Banca (obrigatória — rádio não é nativo-testável):**
  - 2 nós: botoeira num nó → LED pisca → toggla saída no outro nó → LED fixo; nova pressão
    desliga → LED apaga. Segunda botoeira noutro nó toggla a mesma saída; LED de ambos segue
    o estado real. Falha (nó alvo fora) → LED expira p/ apagado.
  - 1 nó (gateway standalone): botoeira local do gateway toggla saída local; LED local segue.
  - Estado compartilhado: scheduler liga a saída → LED da botoeira acende sem pressão.
- **CI:** build ESP32 obrigatório (endpoints webserver-guarded + pinos de LED no variant não
  compilam no nativo).
