# Fase 5b — Captive portal do nó (§7.2) — Design

> Spec de referência: `docs/superpowers/plans/especificacao-irrigacao-mesh.md` §7.2, §8.4, §8.6, §8.7.
> Antecessora: Fase 5a (painel web do gateway) — `docs/superpowers/specs/2026-07-15-irrigacao-fase5a-painel-gateway-design.md`.
> Branch: `sistema-irrigacao`.

## Objetivo

Servir, em **qualquer** nó de irrigação (não só o gateway), um captive portal de campo levantado
pelo botão físico: AP Wi-Fi local + DNS cativo, com três abas — **Este nó**, **Rede** e **Instalador** —
alimentadas por endpoints JSON `/api/portal/*`. Reusa a infra web do Meshtastic (`HTTPServer`) e mantém
toda a lógica de serialização/parsing/ciclo-de-vida numa camada pura testada nativamente, espelhando a
arquitetura da Fase 5a.

Diferença central vs. Fase 5a: o painel 5a é **só GATEWAY** e serve tabelas do gateway; o portal 5b roda
em **todos os papéis**, foca no estado local do próprio nó, e permite comandar a fazenda inteira por rádio
a partir de um nó de campo.

## Decisões de escopo (confirmadas com o usuário 2026-07-17)

1. **Três abas nesta fase**: Este nó + Rede + Instalador. A aba Instalador (§8.4) é antecipada do roadmap
   Fase 8 por decisão explícita.
2. **Ciclo de vida do AP = máquina de estados pura + cola fina**. O `PortalSession` (timers/estados) é puro e
   testado no host; as chamadas reais `softAP`/`DNSServer`/mDNS ficam numa cola só-ESP32 dirigida por ele.
3. **Autorização = AP + PIN apenas**. Estar no AP WPA2 com o PIN de aplicação (§7) é a barreira; sem token
   por-comando. A posse física do botão que levantou o AP é a autorização (spec §7.2).
4. **Rede roteia pelo gateway**. Um nó de campo envia UMA mensagem nova (`MSG_REMOTE_CMD`) ao gateway; o
   gateway valida contra suas tabelas de zonas/estações e re-emite pelo caminho autorizado existente
   (`gwSendValveCmd`). O gateway continua a única autoridade; nenhuma estação precisa confiar em nós
   arbitrários.

## Arquitetura — três camadas (espelha 5a)

### Camada A — C++ puro (testado no host, sem Arduino/WiFi/HTTP/FSCom)

- **`PortalSession`** (`src/modules/irrigation/PortalSession.h`/`.cpp`) — máquina de estados do ciclo de vida
  do AP. Estados: `CLOSED` → `OPEN` (contagem de 10 min, §8.7) → auto-desliga por inatividade. Interface pura:
  - `void requestOpen(uint32_t nowMs)` — botão (pressão curta, nó pareado) ou auto-open de fábrica.
  - `void noteClient(uint32_t nowMs, bool anyClient)` — reseta o timer de inatividade enquanto houver cliente.
  - `void tick(uint32_t nowMs)` — avança estados.
  - `bool apShouldBeUp() const` — saída consumida pela cola.
  - `uint32_t secondsLeft(uint32_t nowMs) const` — para exibição no portal.
  - Timeout do AP: 10 min sem cliente (spec §7.2, tabela §10). Constante compilada.
- **`PortalApi`** (`src/modules/irrigation/PortalApi.h`/`.cpp`) — serializadores + parsers JSON das 3 abas.
  Reusa `IrrigationWeb::JsonWriter`/`JsonReader` (não reimplementar). Funções:
  - `buildNodeState(const NodeStateCtx&, buf, cap)` — aba "Este nó".
  - `parseNodeConfigPatch(json, len, NodeConfigPatch&)` / `buildNodeConfig(...)` — coordenadas locais.
  - `parsePulseTest(json, len, PulseReq&)` — teste de pulso local.
  - `parseNetCommand(json, len, RemoteCmd&)` — comando remoto (zona id, ação, duração).
  - `buildRoster(const RosterCtx&, buf, cap)` — alvos conhecidos (zonas no gateway; nós ouvidos na estação).
  - `buildSurveyStats(const LinkStat&, buf, cap)` — leitura viva do Instalador.
- **`LinkStat`** (`src/modules/irrigation/LinkStat.h`/`.cpp`) — acumulador puro do Instalador. Alimentado com
  `(enviados, reconhecidos, snrQuarterDb, rssi)`; expõe perda %, último/min/max SNR·RSSI, contagem de pacotes.
- **Protocolo** (`IrrigationProtocol.h`/`.cpp`):
  - `MSG_REMOTE_CMD = 12` — corpo `{ uint8_t zoneId; uint8_t action; uint16_t durationS; }`. Encode/decode.
  - `MSG_PING_SURVEY = 10` (já enumerado, sem codec) ganha encode/decode. Corpo: `{ uint32_t seq; int32_t lat;
    int32_t lon; }` (coords opcionais = 0). O eco **reusa `MSG_ACK`** (gateway responde com `Ack{ackedSeq=seq}`)
    — sem tipo de resposta novo.

### Camada B — cola ESP32 (validada por CI, sob `#if !MESHTASTIC_EXCLUDE_WEBSERVER`)

- **`PortalAp`** (`src/modules/irrigation/PortalAp.h`/`.cpp`) — levanta softAP (`Irrigacao-<nome>`, WPA2 + PIN),
  `DNSServer` para redirecionamento cativo, mDNS (`http://<nome>.local`). Dirigido por `PortalSession`
  (`apShouldBeUp()`): sobe/derruba AP conforme a SM. Botão pressão-curta → `session.requestOpen(millis())`.
  Nota de integração: convive com o `WiFiAPClient` existente do Meshtastic; o portal só levanta AP quando a SM
  pede e o Wi-Fi não está já em modo estação configurado pelo usuário (resolver conflito de modo na cola).
- **`IrrigationPortalEndpoints`** (`src/modules/irrigation/IrrigationPortalEndpoints.h`/`.cpp`) — registra as
  rotas `/api/portal/*` no `HTTPServer`, liga parse→aplicar→responder. Mesmo idioma de `IrrigationWebEndpoints`
  (`readBody`, `sendJson`, `sendParseErrors`). Registrado em `ContentHandler.cpp` junto de
  `registerIrrigationHandlers` (antes do catch-all `nodeRoot`).
- **Cola no módulo** (`IrrigationModule.cpp`/`.h`):
  - Acessores de estado local do nó (`portalNodeState()`), patch de config local, pulso local.
  - Enfileira TX de `MSG_REMOTE_CMD` (estação → gateway) e de `MSG_PING_SURVEY` (Instalador).
  - **No gateway**: handler de `MSG_REMOTE_CMD` → valida zona → `gwSendValveCmd`; responde `MSG_PING_SURVEY`
    com `MSG_ACK`.
  - Alimenta `LinkStat` a partir do `mp.rx_snr`/`rx_rssi` dos acks de survey recebidos.

### Camada C — frontend estático (LittleFS)

`data/irrigacao/portal/index.html`, `app.js`, `style.css`. Reusa o `style.css` da Fase 5a onde possível.
Três abas consumindo `/api/portal/*` via `fetch()`. HTML/CSS/vanilla-JS, sem framework.

## Endpoints `/api/portal/*`

| Método | Rota | Ação |
|---|---|---|
| GET | `/api/portal/node` | Estado do nó: bateria, painel V, estados de válvula/GPO, tamper, safe-mode, papel, nome, gateway vinculado, epoch, coords |
| POST | `/api/portal/node/config` | Patch de config local (coordenadas) |
| POST | `/api/portal/node/pulse` | Teste de pulso local (ValveController; teto de 120 min respeitado) |
| POST | `/api/portal/net/command` | Comando remoto → relay pelo gateway (`MSG_REMOTE_CMD{zoneId, action, durationS}`) |
| GET | `/api/portal/net/roster` | Alvos: tabelas reais de zona/estação no gateway; lista de nós ouvidos na estação |
| POST | `/api/portal/survey/start` | Inicia survey do Instalador (dispara pings periódicos) |
| GET | `/api/portal/survey/stats` | Leitura viva de qualidade do enlace (`LinkStat`) |

## Simplificações (YAGNI)

- **Rede por número de zona, não nome**. Uma estação de campo não tem cópia dos nomes de zona do gateway;
  envia zona-por-número, o gateway valida/mapeia. Sincronização de nomes/roster fica para fase futura.
- **Eco do Instalador reusa `MSG_ACK`**. Gateway reconhece cada `PING_SURVEY`; perda = acks faltantes,
  SNR/RSSI de `mp.rx_snr`/`rx_rssi`. Sem tipo de resposta novo.
- **Auth = AP+PIN apenas**. `MSG_REMOTE_CMD` aceito de qualquer nó com a PSK da fazenda — o gateway é a
  autoridade e valida o id da zona.

## Restrições globais (herdadas da spec, valem em toda fase)

- Teto absoluto de abertura **120 min** compilado (`IrrigationProto::MAX_OPEN_SECONDS`); o portal nunca o
  contorna (pulso local e comando remoto ambos limitados).
- Fail-safe local sempre; versão de protocolo em toda mensagem; mismatch = rejeição segura.
- Payload de rádio ≤ ~200 bytes (`IrrigationProto::MAX_PAYLOAD`).
- Toda a camada A é **pura**: só headers de irrigação já usados pela `IrrigationWebApi` + `<cstdint>` etc.
  Nunca `Arduino.h`, WiFi, HTTP, `FSCom`.
- Cola HTTP/AP (`IrrigationPortalEndpoints.cpp`, `PortalAp.cpp`) sob `#if !MESHTASTIC_EXCLUDE_WEBSERVER`;
  excluída do build nativo via `portduino.ini` src_filter (mesmo mecanismo de `IrrigationWebEndpoints.cpp`).
- Portal disponível em todos os papéis; o **relay `MSG_REMOTE_CMD` só é tratado no gateway**.
- Testes: `./bin/run-tests.sh` GREEN. Novas suítes nativas atualizam `test/native-suite-count` no mesmo commit.
- Formatar com `trunk fmt` antes de cada commit (rodado em CI — não instalado localmente).
- Idioma: comentários em português, identificadores como nos arquivos vizinhos.

## Sequenciamento sugerido (para o plano)

Marco 1 (shippable): **AP shell + Este nó**. `PortalSession` + `PortalAp` + `PortalApi::buildNodeState`/pulse/
config + frontend aba "Este nó". Portal de campo funcional para diagnóstico local em qualquer nó.

Marco 2: **Rede**. `MSG_REMOTE_CMD` codec + handler no gateway + `parseNetCommand`/`buildRoster` + aba "Rede".

Marco 3: **Instalador**. `MSG_PING_SURVEY` codec + eco via ACK + `LinkStat` + endpoints survey + aba "Instalador".

Cada marco termina GREEN e é integrável isoladamente.

## Suítes nativas novas

- `test/test_portal_session/` — máquina de estados do AP.
- `test/test_portal_api/` — build/parse das 3 abas.
- `test/test_linkstat/` — acumulador de qualidade de enlace.
- Codecs de protocolo (`MSG_REMOTE_CMD`, `MSG_PING_SURVEY`) entram na suíte existente `test_irrigation_protocol`.

`test/native-suite-count`: 44 → 47 (três suítes novas).

## Follow-ups conhecidos (deferidos, seguros)

- Sincronização de nomes de zona/roster para nós de campo (Rede mostra números).
- §8.5 survey no gateway (registro de beacons PING_SURVEY × qualidade para planejamento) — o codec entra aqui,
  a agregação/tela no painel do gateway fica para fase futura.
- Modo instalador com vizinhos além do gateway (§8.4 menciona "vizinhos visíveis") — MVP mede só o enlace com
  o gateway.
- Resolução fina do conflito de modo Wi-Fi (portal AP vs. `WiFiAPClient` em modo estação) pode exigir ajuste
  na cola após validação de hardware.
