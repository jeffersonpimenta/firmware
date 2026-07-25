# Irrigação — Fase 8c: Portal do device SERVICO (Clientes · Rede · Log + vias portal import/export)

**Design doc — 2026-07-25**
Spec de origem: `especificacao-irrigacao-mesh.md` §11.8 (interface) + §11.7 (vias portal do import/export), apoiado em §11.2–§11.6. Roadmap: `2026-07-11-irrigacao-roadmap.md` (Fase 8). Terceiro recorte da Fase 8 — depende do núcleo entregue na 8b (`2026-07-25-irrigacao-fase8b-device-servico-nucleo-design.md`) e do protocolo da 8a.

## 1. Contexto e recorte

A 8a colocou os pré-requisitos de protocolo no parque (§11.9). A 8b entregou o **núcleo do device SERVICO** sem UI: cofre de clientes (`ServiceVault`/`IProfileStore`/`LittleFsProfileStore`), codec de backup (`ServiceBackup`), re-tune de canal + reboot, *emissor* de varredura, decisão de rota de config, requester RESYNC e o export do gateway (`GET /api/irrigation/export`). Tudo o que existe hoje é dirigível só por botão/console serial.

A **8c** entrega a **interface** (§11.8): o portal Wi-Fi do device ganha, no role `SERVICO`, três abas — **Clientes**, **Rede do cliente ativo**, **Log** — e fecha as **vias de portal do import/export** (§11.7: `GET /export`, `POST /import`) que a 8b adiou por dependerem do portal.

**Decisões de recorte (usuário, 2026-07-25):**

- **Editor de config por nó = campos completos.** A aba "Rede" renderiza o blob de settings v5 inteiro da estação (pinos/zonas/intertravamentos/sensores/limiares/polaridade) para edição remota, gravando pelas 2 rotas do §11.6. (Alternativa "clonar/empurrar" e "só leitura" rejeitadas.)
- **Via cartão-SD adiada.** A 8c faz portal (`GET /export`, `POST /import`) + o USB serial já veio na 8b. `import.json` no boot via SD é follow-on (formato do envelope já é compatível; entra depois sem retrabalho).

Escopo **IN** da 8c:

1. **Aba Clientes** (§11.8) — lista + seleção do cliente ativo (dispara re-tune + reboot da 8b), varredura (dispara PROBE + mostra respondentes), import/export via portal.
2. **Aba Rede do cliente ativo** (§11.8, §11.6) — nós detectados com estado (bateria, epoch, role, versão, SNR); por nó: **ler config**, **escrever config (editor cheio, 2 rotas)**, teste de pulso, acionar zona, aprovar pareamento, `RESYNC_SEQ`.
3. **Aba Log** (§11.8) — `servico.jsonl` local (uptime + timestamp adotado do gateway; device sem RTC). **Inclui o writer do log**, que a 8b não implementou (só previu o arquivo no layout do cofre).
4. **Vias portal do import/export** (§11.7) — `GET /api/portal/service/export` (despeja o envelope multi-cliente do cofre) e `POST /api/portal/service/import` (upload → staging → validate → merge por id, `?replace=1`).

Escopo **OUT** (adiado, com destino; formato permanece compatível):

- **Via cartão-SD** (`import.json` na raiz, aplicado no boot) → follow-on.
- **Cifra por senha da PSK** no export (§5.5) → follow-on (plaintext, igual 8b; posse-da-PSK=autoridade §11.1).
- **Import no gateway** (reprovisionar substituto a partir de backup) → follow-on (já OUT na 8b).
- **Beacon de site survey** (§8.5) → **8d**.
- **Varredura multi-cliente com reboot-sweep** (cursor persistido entre reinícios) → follow-on (8b/8c fazem só o cliente ativo).

**Sem mudança na ABI de estação** (settings v5, 176 B, intacto). **Protocolo VERSION permanece 1** — a 8c não introduz tipo de mensagem novo; reusa GET/SET_CONFIG fragmentado (Fase 2), pulso/zona (Fase 1/6a), aprovação de pareamento (Fase 3), `RESYNC_SEQ`/`PING_SURVEY` (8a) e os executores do 8b, todos marcados `FLAG_FROM_SERVICE`.

## 2. Decisões desta fase

| # | Decisão | Escolha |
|---|---|---|
| Q1 | Profundidade do "escrever config" na aba Rede | **Editor de campos completo** — settings v5 inteiro editável remoto; grava pelas 2 rotas (§11.6). |
| Q2 | Via cartão-SD do import (§11.7) | **Adiada** — 8c faz portal + o USB serial já é da 8b. SD é follow-on. |
| Q3 | Onde mora o código puro do portal SERVICO | **TU nova `ServicePortalApi.h/.cpp`** (`namespace IrrigationWeb`), suite nova `test_service_portal`. Não incha `PortalApi` (node-portal). Segue o split do repo. |
| Q4 | Endpoints ESP32 | **TU nova `ServicePortalEndpoints.cpp`** (`#if !MESHTASTIC_EXCLUDE_WEBSERVER`), rotas `/api/portal/service/*`, gated `role==SERVICO`. |
| Q5 | Shell/AP do portal | **Reusa o shell existente** (5b: `PortalAp`/`PortalSession`/frontend `data/irrigacao/portal/`). Tabs SERVICO condicionadas por `role`. |
| Q6 | Mecânica do upload de import | **Stream do corpo POST → arquivo de staging no LittleFS → validate → merge.** Evita o envelope inteiro na RAM; teto de tamanho documentado. |
| Q7 | Campos geridos vs editáveis no editor | **Geridos (read-only):** `magic`, `version`, `configEpoch` (a rota cuida do epoch), `role` (trocar role remoto é fora de escopo). **Editáveis:** o resto (pinos, thresholds, sensores, intertravamentos, coords, polaridade). |
| Q8 | Writer do `servico.jsonl` | **Novo, nesta fase** — `ServiceController` anexa uma linha por ação de serviço; leitor puro faz tail. Uptime + timestamp adotado do gateway (sem RTC). |

## 3. Compatibilidade e modelo de ameaça

**Aditividade total.** A 8c não muda o wire nem a ABI; é UI + glue web sobre o núcleo do 8b. Nenhum novo tipo de mensagem, nenhum bump de VERSION, nenhum bump de settings.

**Autoridade = posse da PSK (§11.1).** O `GET /export` do portal despeja o envelope multi-cliente **em claro** (PSKs de todos os clientes). Mesmo modelo do 8b: quem opera o device tem controle administrativo dos parques cujos perfis ele contém; arquivos exportados são segredo do operador. Cifra por senha adiada.

**Superfície de rede do portal.** O portal SERVICO abre exatamente as mesmas rotas administrativas por Wi-Fi (softAP WPA2 + captive, 5b) que o node-portal já abre — o AP fica no ar apenas na janela do portal (`PortalSession`, gesto físico), não permanentemente. As rotas `/api/portal/service/*` só respondem quando `role==SERVICO`.

**Re-tune reinicia o device.** `POST /select` chama `planRetune` (8b) + reboot. O AP Wi-Fi do portal é **independente do canal LoRa** — reescrever o `channelFile` e reiniciar não derruba o portal permanentemente; ele volta após o boot. O frontend mostra "re-tunando, aguarde reboot" e re-poll. Teto fail-safe de 120 min (§4.2) inalterado.

**Airtime.** Toda emissão (PROBE, GET/SET_CONFIG, pulso, zona, pareamento, RESYNC) passa pelo `RateLimiter` existente e é marcada `FLAG_FROM_SERVICE`. A gravação de config direta grava `epoch+1` e registra pendência; o gateway adota depois pela regra do maior epoch (8a).

## 4. Componentes

### 4.1 `ServicePortalApi` — camada pura (§11.8) — **nativo-testável**

TU nova, sem I/O; recebe dados como parâmetro (views/structs), nunca lê tabela/arquivo/rádio direto. `namespace IrrigationWeb`, reusa `JsonWriter`/`JsonReader`/`ParseResult` de `IrrigationWebApi`.

**Aba Clientes:**
- `size_t buildClientList(const LightProfile *clients, size_t n, const char *activeId, char *buf, size_t cap)` — id, nome, canal (nome/preset), gateway, `active` bool.
- `ParseResult parseSelect(const char *json, size_t len, char *idOut, size_t idCap)` — `{ "id": "fazenda-sp-01" }`.

**Aba Rede — roster + scan:**
- `size_t buildScanResults(const ScanResults &scan, char *buf, size_t cap)` — mapeia `ScanEntry` (node, role, epoch, vbatCentiV, fwVersion, latE7, lonE7, snrQuarterDb) para JSON.

**Aba Rede — editor de config (peça central):**
- `size_t buildStationConfig(const IrrigationSettings &s, char *buf, size_t cap)` — serializa o blob v5 inteiro para JSON de edição. Campos geridos marcados/omitidos para edição (`configEpoch`, `magic`, `version` só informativos; `role` read-only).
- `ParseResult parseStationConfig(const char *json, size_t len, IrrigationSettings &out)` — reconstrói o blob a partir do JSON do editor; **preserva** os campos geridos do `out` de entrada (epoch/magic/version/role vêm do blob lido, não do formulário).
- **Round-trip identity** é o teste-âncora: `parseStationConfig(buildStationConfig(s)) == s` (byte-a-byte no blob de 176 B), incluindo sensores e intertravamentos.

**Aba Rede — ações e requisições:**
- `ParseResult parseNodeConfigReq(const char *json, size_t len, NodeConfigReq &out)` — `{ node, route: "gateway"|"direct", config:{…} }`.
- `ParseResult parseNodeAction(const char *json, size_t len, NodeAction &out)` — `{ node, action: "pulse"|"zone"|"approve_pair"|"resync", … }` (reusa semântica de `parsePulse`/`parseNetCommand` de `PortalApi` para os campos de pulso/zona).

**Aba Log:**
- `size_t buildServiceLog(IServiceLogReader &reader, char *buf, size_t cap)` — tail das últimas N linhas de `servico.jsonl`; `reader` é interface injetável (glue LittleFS na produção, duplê em RAM no teste).

**Import/export:** sem código puro novo — reusa `ServiceVault::exportEnvelope` / `ServiceVault::importEnvelope` (já puros, 8b). O endpoint só faz o transporte HTTP.

### 4.2 `ServicePortalEndpoints.cpp` — glue ESP32 (só-webserver)

TU nova sob `#if !MESHTASTIC_EXCLUDE_WEBSERVER`, espelha `IrrigationPortalEndpoints.cpp` (helpers `sendJson`/`sendParseErrors`/`readBody`). Todo handler começa com `if (!irrigationModule || !irrigationModule->svcIsService()) { 404 }`. Rotas:

| Rota | Método | Handler → glue do módulo |
|---|---|---|
| `/api/portal/service/clients` | GET | `svcPortalListClients` → `buildClientList` |
| `/api/portal/service/select` | POST | `parseSelect` → `svcPortalSelect` (planRetune 8b → persist active → **reboot**, responde 202) |
| `/api/portal/service/scan` | POST | `svcPortalStartScan` (emite PROBE, 8b) |
| `/api/portal/service/scan` | GET | `buildScanResults(controller.scanResults())` |
| `/api/portal/service/export` | GET | `vault.exportEnvelope` (heap; plaintext) |
| `/api/portal/service/import` | POST | stream body → staging file → `vault.importEnvelope` (`?replace=1` → replace) |
| `/api/portal/service/node/config` | GET | `svcPortalReadConfig(node)` (GET_CONFIG direto, Fase 2) → `buildStationConfig` |
| `/api/portal/service/node/config` | POST | `parseNodeConfigReq` → `svcPortalWriteConfig` (decideConfigRoute 8b + SET_CONFIG fragmentado Fase 2) |
| `/api/portal/service/node/action` | POST | `parseNodeAction` → `svcPortalNodeAction` (pulso/zona/aprovar-par/resync) |
| `/api/portal/service/log` | GET | `buildServiceLog` (heap, ~10 KB, igual `hLog`) |

Registro em `registerIrrigationServicePortalHandlers(HTTPServer*)`, chamado ao lado de `registerIrrigationPortalHandlers` no boot do webserver.

### 4.3 Glue no `IrrigationModule` (compila nativo + banca)

Accessors `svcPortal*` que traduzem requisição web → intents do `ServiceController`/`ServiceVault` (8b) + transporte de rádio:

- `svcIsService()` — `role == SERVICO`.
- `svcPortalListClients(...)` / `svcPortalSelect(id)` — cofre + `planRetune` + `applyRetune` (8b) + reboot.
- `svcPortalStartScan()` — `svcEmitProbe` (8b); resultados já acumulam via intake de PING REPLY (8b).
- `svcPortalReadConfig(node)` — envia GET_CONFIG direto; ao chegar o reply (reassembler + CRC, Fase 2) migra o blob (`migrateIrrigationSettings`) e o disponibiliza ao endpoint.
- `svcPortalWriteConfig(node, route, blob)` — `decideConfigRoute` (8b); VIA_GATEWAY → SET_CONFIG ao gateway; DIRECT → SET_CONFIG à estação com `epoch+1` + registra pendência no perfil.
- `svcPortalNodeAction(node, action, params)` — pulso/zona (reusa caminhos existentes marcados `FLAG_FROM_SERVICE`), aprovar pareamento (PAIR_GRANT, Fase 3), `RESYNC_SEQ` (8b).
- **Service log:** cada ação acima chama `ServiceController::logService(evento, node)` que anexa a `/log/servico.jsonl`.

### 4.4 Service log `servico.jsonl` (novo — writer + reader)

- **Writer (glue):** `ServiceController::logService(...)` compõe uma linha JSONL `{ "up": <uptimeS>, "ts": <gwTsOrNull>, "ev": "...", "node": "!x", … }` e anexa via `IProfileStore`/FSCom a `/log/servico.jsonl` (append-only; rotaciona/trunca acima de um teto de linhas). Sem RTC: `ts` é o último timestamp adotado do gateway (via heartbeat/ACK) ou `null`.
- **Reader (pura + glue):** `IServiceLogReader` (interface injetável) devolve as últimas N linhas; `buildServiceLog` as embrulha em array JSON. Impl LittleFS lê o tail do arquivo; duplê em RAM no teste.
- Autoridade temporal fica no gateway na mesclagem com sua auditoria (§11.8) — o device só carimba uptime + ts adotado.

### 4.5 Frontend `data/irrigacao/portal/`

`GET /api/portal/node` já devolve `role`. Quando `role == SERVICO`, o `app.js` mostra as abas **Clientes / Rede / Log** e esconde as abas de node/estação; caso contrário, comportamento atual (5b). Sem bundle novo — mesmo shell, tabs condicionais (padrão do 7b).

- **Clientes:** lista (poll `/clients`), botão *Selecionar* (→ `/select`, aviso de reboot + re-poll), *Varredura* (→ `POST /scan`, depois poll `GET /scan`), *Exportar* (download de `/export`), *Importar* (upload para `/import`, checkbox replace).
- **Rede:** tabela de nós (poll `/scan`); por nó — *Ler config* (abre editor preenchido de `/node/config`), editor de campos (grava via `POST /node/config` com seletor de rota gateway/direta), *Pulso*, *Zona*, *Aprovar pareamento*, *RESYNC* (`/node/action`).
- **Log:** poll `/log`, render append-only (uptime + ts).

## 5. Arquitetura / arquivos tocados

| Arquivo | Mudança |
|---|---|
| `ServicePortalApi.h/.cpp` (novo) | camada pura: buildClientList/parseSelect/buildScanResults/**buildStationConfig+parseStationConfig**/parseNodeConfigReq/parseNodeAction/buildServiceLog |
| `ServicePortalEndpoints.h/.cpp` (novo) | glue ESP32 `/api/portal/service/*`, gated SERVICO; `registerIrrigationServicePortalHandlers` |
| `ServiceController.h/.cpp` | `logService(...)` (writer do `servico.jsonl`); expõe o reader de log |
| `IProfileStore.h` / `LittleFsProfileStore` | append + tail de `/log/servico.jsonl`; impl do `IServiceLogReader` |
| `IrrigationModule.h/.cpp` | accessors `svcPortal*` (list/select/scan/readConfig/writeConfig/nodeAction) + intake do GET_CONFIG reply; chama `registerIrrigationServicePortalHandlers` no boot do webserver |
| `data/irrigacao/portal/index.html` · `app.js` | tabs Clientes/Rede/Log condicionadas a `role==SERVICO` |
| `test/test_service_portal/` (novo) | suite nativa da camada pura |

Sem ABI de settings, sem tipo de wire novo, sem bump de VERSION.

## 6. Testes

**Restrição do repo:** nenhum teste nativo instancia `IrrigationModule` (depende do stack mesh). Padrão do repo: **lógica pura nativa-testada; glue de handler/rádio validado por compilação nativa + banca de hardware**.

Suite nova `test_service_portal` (TDD no plano):
- **`buildStationConfig`/`parseStationConfig` round-trip** — âncora: blob v5 arbitrário → JSON → blob idêntico (176 B byte-a-byte), cobrindo sensores (4), intertravamentos (4), pinos negativos (slot vazio), polaridade, coords. Campos geridos (epoch/magic/version/role) preservados do blob de entrada, não do formulário.
- **`buildClientList`** — inclui flag `active` correto; lista vazia.
- **`parseSelect` / `parseNodeConfigReq` / `parseNodeAction`** — casos válidos + rejeições (id ausente, rota inválida, ação desconhecida, campos faltando).
- **`buildScanResults`** — mapeia todos os campos do `ScanEntry`; scan vazio.
- **`buildServiceLog`** — tail de N linhas sobre duplê de reader em RAM; log vazio; truncagem no teto.

Glue (compila nativo + banca, sem TDD nativo): endpoints ESP32, GET/SET_CONFIG por rádio, upload streaming, reboot no select, writer real do `servico.jsonl`, tabs do frontend.

Suite nativa completa deve passar de **897 → ~910+ GREEN** (1 suite nova; sem mudança nas existentes). Confirmação via Docker (memória `windows-native-test-docker`).

## 7. Critérios de aceite

1. Portal em `role==SERVICO` mostra as abas Clientes/Rede/Log; rotas `/api/portal/service/*` respondem 404 em outros roles.
2. Aba Clientes lista o cofre, *Selecionar* re-tuna + reinicia no canal do cliente; portal volta pós-boot.
3. *Varredura* dispara PROBE e a tabela mostra respondentes (node/role/epoch/vbat/fw/coords/SNR).
4. *Ler config* de um nó preenche o editor; editar + *Gravar* via-gateway incrementa o epoch pelo gateway; via-direta grava `epoch+1` + pendência (gateway adota depois pela regra do maior epoch).
5. `buildStationConfig`/`parseStationConfig` fazem round-trip idêntico do blob v5.
6. *Exportar* baixa o envelope multi-cliente; *Importar* (upload) valida em staging e faz merge por id (`replace` opcional) — round-trip com o export.
7. Aba Log mostra `servico.jsonl` (uptime + ts adotado).
8. ABI settings v5 e protocolo VERSION 1 intactos; suite nativa ~910 GREEN.

## 8. Fora de escopo (adiado, com destino)

- Via cartão-SD (`import.json` no boot) — follow-on.
- Cifra por senha da PSK no export (§5.5) — follow-on.
- Import no gateway (reprovisionar substituto) — follow-on.
- Beacon de site survey §8.5 — **8d**.
- Varredura multi-cliente com reboot-sweep (cursor persistido) — follow-on.

**Banca 2+ nós EXIGIDA antes de campo** — GET/SET_CONFIG por rádio, re-tune, varredura e as 2 rotas de config entre binários não são nativo-testáveis (rádio real).
