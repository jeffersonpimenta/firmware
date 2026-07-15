# Fase 5a — Painel web do gateway (design)

Spec-mãe: `myfork/especificacao-irrigacao-mesh.md` (v0.1), §7.1.
Roadmap: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (Fase 5).
Mockup visual: `myfork/Irrigacao Mobile.dc.html` (Design Canvas, 2026-07-11).

## Contexto e recorte

A Fase 5 do roadmap agrupa dois subsistemas independentes — painel web do gateway
(§7.1) e captive portal do nó (§7.2) — mais a infraestrutura WiFi/HTTP/LittleFS e o
frontend. Grande demais para um único plano. Este documento cobre **somente a Fase
5a: o painel web do gateway**. O captive portal do nó vira a Fase 5b.

Fora de escopo (adiado):

- Captive portal / UI do nó em campo (§7.2) → Fase 5b.
- Telas de Grupos & Intertravamentos e Log de auditoria (§8.9–§8.13) → Fase 6.
- Tela Sistema: PIN, backup, exportar chave da fazenda (§11) → Fase 8.
- Modo instalador / site survey (§8.4, §8.5) → futuro.

As telas adiadas que já aparecem no mockup são renderizadas desabilitadas ("em
breve") na barra de abas, para não quebrar a navegação.

## Restrição de teste (decisiva para a arquitetura)

A suíte nativa é C++ de host (`./bin/run-tests.sh`). WiFi, HTTP (`esp32_https_server`)
e LittleFS são exclusivos do ESP32 — não compilam no host. Logo o código se separa em:

- **Camada pura** (view-models JSON + parsers/validadores): sem dependência de
  Arduino/WiFi/HTTP → compila e roda no host, com testes nativos. É o grosso do valor
  e do risco lógico.
- **Cola HTTP** (registro de rotas, WiFi AP, serviço de arquivos estáticos): só ESP32
  → validada por build de CI, nunca localmente (consistente com as fases anteriores;
  o build tbeam/gateway nunca rodou local).

Este é o mesmo padrão das fases 1–4: classes pequenas e puras com teste nativo, e a
cola de mesh/hardware isolada em `IrrigationModule.cpp`.

## Decisão de abordagem

Três opções para servir o painel:

- **(A) Reusar a infra web do Meshtastic** — `esp32_https_server` +
  `src/mesh/http/ContentHandler.cpp` já sobem HTTPServer, servem arquivos estáticos do
  `FSCom` (LittleFS) e expõem `/api/v1/*` via `ResourceNode`. Adicionamos nós
  `/api/irrigation/*` e servimos o painel do LittleFS. **Escolhida.**
- (B) Servidor HTTP dedicado da irrigação — duplica WiFi + HTTPS + FS já em árvore.
- (C) UI externa — fora do escopo (o gateway serve a própria UI, §7.1).

A wins: reuso, menos superfície nova, menos código exclusivo de ESP32.

JSON: **ArduinoJson** (já é dependência do projeto — `MeshPacketSerializer.cpp`;
header-only, compila no host), usado tanto na serialização quanto no parsing, o que
mantém a camada pura testável nativamente.

Gating: tudo ativo apenas quando `IrrigationSettings.role == IrrigationRole::GATEWAY`
(valor 1) e sob `#if !MESHTASTIC_EXCLUDE_WEBSERVER` em plataforma ESP32.

## Componentes

### 1. `IrrigationWebApi` — camada pura (teste nativo)

Sem Arduino/WiFi/HTTP. Lê o estado em RAM produzido na Fase 4 (StationRegistry,
GatewayTables/ZoneTable, ProgramScheduler, AlertCenter, StationMonitor) e produz/consome
JSON.

Serializadores (estado → JSON):

- `buildOverview()` — contadores (estações, em execução, alertas), execução em curso,
  próximas execuções, alertas não reconhecidos, resumo de pareamento pendente, estado
  "sem relógio" quando o gateway não tem RTC.
- `buildStations()` — por estação: nome, nó, último contato, bateria/painel, SNR,
  epoch, reboots, coordenadas, estado composto de sincronização
  (`sincronizada`/`pendente`/`inalcançável`) e flags (tamper, modo seguro, energia).
- `buildZones()` — roteamento zona→(nó, saída), `padrao_min`, `max_min`, tipo
  (válvula/GPO), fonte física opcional (modo híbrido), estado de bloqueio/execução.
- `buildPrograms()` — dias, horário, sequência de zonas×duração, ativo/pausado,
  execução em curso.

Parsers + validadores (JSON → struct validada + lista de erros):

- `parseZoneUpsert`, `parseZoneDelete`
- `parseProgramUpsert`, `parseProgramToggle`
- `parseCommand` — teste de pulso / abrir / fechar / reconhecer alerta / aprovar
  pareamento.

Cada parser reaproveita a validação de `GatewayTables` (upsert) e o codec `SET_CONFIG`
existentes; devolve um resultado que a cola HTTP converte em 200 ou 400. Nunca aplica
efeito colateral — só valida e monta a struct.

Nova suíte: `test_irrigation_webapi`.

### 2. `IrrigationWebEndpoints` — cola HTTP (só ESP32 / CI)

- `registerIrrigationHandlers(HTTPServer*)` chamado de `ContentHandler::registerHandlers`,
  guardado por `role == GATEWAY`.
- Nós:
  - `GET  /api/irrigation/overview`
  - `GET  /api/irrigation/stations`
  - `GET|POST /api/irrigation/zones`
  - `POST /api/irrigation/zones/delete`
  - `GET|POST /api/irrigation/programs`
  - `POST /api/irrigation/command`
- Fluxo POST: corpo → `IrrigationWebApi` parse → aplica via
  `IrrigationGateway`/`IrrigationModule` (que emite o `SET_CONFIG`/`CMD` por LoRa e
  registra retries + epoch no `CommandTracker`) → responde a view JSON. GET apenas
  serializa.
- Painel estático servido do LittleFS pelo handler genérico de arquivos do `FSCom` já
  existente.

### 3. Frontend — assets estáticos em `data/` → LittleFS

- HTML/CSS/JS puro (vanilla), reproduzindo a linguagem visual do mockup (paleta oklch,
  cards, chips, barra de abas inferior) para as telas 5a: **Visão Geral · Estações ·
  Zonas (lista + edição) · Programas (lista + edição)**.
- `fetch()` faz polling de overview/stations (~3 s para estado vivo); formulários fazem
  POST; a convergência é exibida a partir do campo `sync` de cada estação.
- Desacoplado (§3.3): trocar só os arquivos do LittleFS muda a UI sem reflashar a
  aplicação.
- Abas adiadas (Grupos & Intertravamentos, Log, Sistema) aparecem desabilitadas.

## Fluxo de dados (exemplo: editar zona)

1. Navegador `POST /api/irrigation/zones` com o JSON da zona.
2. Endpoint → `IrrigationWebApi::parseZoneUpsert` → `ZoneEntry` validada.
3. `IrrigationGateway` aplica em `GatewayTables` e dispara `SET_CONFIG` à estação alvo
   (retries + epoch pelo `CommandTracker`).
4. Resposta imediata `200 { zone, sync: "pendente" }`.
5. Frontend faz polling de `stations`; `sync` vira `sincronizada` ao chegar o ACK com
   o epoch confirmado. "Toda mudança só é dada como aplicada após ACK com epoch
   confirmado" (§7.1).

## Restrição sem-RTC (mantida da Fase 4)

O painel renderiza zonas/programas mesmo sem relógio de parede. A Visão Geral mostra o
estado "sem relógio — cronograma inativo". O modo espelho (só `millis()`) continua
plenamente operante. Reaproveita o idle gracioso da Fase 4.

## Pré-tarefas: pendências herdadas da revisão final da Fase 4

A revisão da Fase 4 deixou 3 pontos Important explicitamente carregados para a Fase 5.
Entram como pré-tarefas porque o painel toca exatamente as mesmas estruturas:

1. Scheduler não suprime o CLOSE quando o modo espelho está ativo na mesma zona (brecha
   de bypass) — corrigir + teste.
2. A varredura de silêncio itera em paralelo com a allowlist; precisa de um iterador
   `StationRegistry::nodeAt` — adicionar (usado também pelo serializador de estações).
3. Sem `static_assert` ligando `StationEntry::blob[52]` a `sizeof(IrrigationSettings)`
   — adicionar (adoção falha em silêncio se o settings crescer).

## Testes

- **Nativo** (`test_irrigation_webapi`): saída dos serializadores (trechos de JSON
  golden), parsers aceitam/recusam (zona inválida, nó desconhecido, sequência de
  programa excedendo o limite, comando despachado), mais os testes das 3 pré-tarefas.
- **CI**: build do gateway ESP32 compila a cola HTTP e gera a imagem do LittleFS (nunca
  compilado localmente).
- **Frontend**: verificação em navegador é manual, adiada (sem harness nativo).

## Invariantes globais preservadas (spec)

Teto absoluto de abertura 120 min compilado; fail-safe local sempre; versão de
protocolo em toda mensagem; mismatch = rejeição segura; convergência por epoch. O
painel apenas expõe e comanda — nunca contorna esses limites.
