# Irrigação — Fase 8b: Device SERVICO núcleo (cofre · re-tune · varredura · config r/w · import/export)

**Design doc — 2026-07-25**
Spec de origem: `especificacao-irrigacao-mesh.md` §11.1–§11.7, §5.4, §5.5, §10. Roadmap: `2026-07-11-irrigacao-roadmap.md` (Fase 8). Segundo recorte da Fase 8 — depende da fundação de protocolo entregue em 8a (`2026-07-25-irrigacao-fase8a-protocolo-parque-design.md`).

## 1. Contexto e recorte

A Fase 8a colocou no parque os **pré-requisitos de protocolo** (§11.9): aceitação de `FLAG_FROM_SERVICE`, `last_seq` por remetente + responder `RESYNC_SEQ`, regra do maior epoch, responder `PING_SURVEY`. A Fase 8b entrega o **device SERVICO em si** (§11.1–§11.7): o gateway itinerante multi-cliente que guarda credenciais de vários clientes, sintoniza o canal de um deles e lê/escreve config dos nós ao alcance, sem internet e sem contato físico.

**Decisão de recorte (usuário, 2026-07-25):** Fase 8 dividida em 8a/8b/8c/8d; a 8b é **combinada** — um único design + plano cobrindo todo o núcleo (cofre + re-tune + varredura + config r/w + import/export + requester RESYNC). A UI (portal do device, §11.8) fica na **8c**; o site survey §8.5 fica na **8d**.

Escopo **IN** da 8b:

1. **Cofre de clientes** (§11.2) — armazenamento LittleFS multi-cliente com parsing em streaming e staging atômico.
2. **Backup completo §5.5 por cliente** (decisão do usuário) — cada perfil é um backup de fazenda inteiro (PSK do canal + todas as tabelas do gateway: estações, zonas, programas, intertravamentos, grupos hidráulicos). Inclui o **export do gateway** que estava adiado desde a 6b/7b ("backup completo com PSK+grupos").
3. **Troca de canal em runtime** (§11.3) — re-tune (nome+PSK+preset) via `channelFile` do Meshtastic, persistência do cliente ativo e **reinício**.
4. **Varredura** (§11.4) — *emissor* da sonda `PING_SURVEY` no **cliente ativo** (single-channel); registra respondentes.
5. **Leitura/escrita de config, 2 rotas** (§11.6) — via gateway (padrão) ou direta à estação (`epoch+1` + pendência); `GET_CONFIG` sempre direto.
6. **RESYNC_SEQ requester** (§11.5) — lado device: quando o contador de um par (cliente, nó) é desconhecido, consulta e retoma em `last_seq+1`.
7. **Import/export** (§11.7) — engine multi-cliente (merge por id + `--replace`) + via **USB serial** (`EXPORT`/`IMPORT`) como caminho de bancada.

Escopo **OUT** (adiado, com destino; formato permanece compatível):

- **Import no gateway** (reprovisionar um gateway substituto *a partir de* um backup) — caminho inverso, toca o load de todas as tabelas + segurança de válvula na importação. Follow-on próprio.
- **UI/portal do device** (§11.8, abas Clientes/Rede/Log) → **8c**. As vias **SD-card** e **portal `GET /export` / `POST /import`** (§11.7) também vão para a 8c (dependem do portal); a 8b entrega o engine puro + USB serial.
- **Cifra por senha da PSK** (§5.5 "PSK cifrada por senha do usuário") — **plaintext** em 8b (decisão do usuário; posse-da-PSK=autoridade §11.1). Cifra é follow-on.
- **Varredura multi-cliente com reboot-sweep** (cursor persistido entre reinícios) — 8b faz só o cliente ativo (decisão do usuário). Beacon de survey §8.5 → 8d.

**Sem mudança na ABI de estação** (settings v5, 176 B, intacto). **Protocolo VERSION permanece 1** — a 8b não introduz nenhum tipo de mensagem novo; reusa os codecs da 8a (`PING_SURVEY`, `RESYNC_SEQ`) e os comandos existentes (`SET/GET_CONFIG` fragmentado da Fase 2, pulso/zona/pareamento), todos marcados `FLAG_FROM_SERVICE`.

## 2. Decisões desta fase

| # | Decisão | Escolha |
|---|---|---|
| Q1 | Split vs combinado da 8b | **Combinado** — um design + plano para todo o núcleo. |
| Q2 | Conteúdo do perfil de cliente / do import-export | **Backup §5.5 completo por cliente** — PSK do canal + todas as tabelas do gateway. Puxa o export-do-gateway adiado para dentro da 8b. |
| Q3 | Cifra da PSK (repouso e export) | **Plaintext em todo lugar.** Posse-da-PSK=autoridade (§11.1); a PSK do canal ativo já reside em claro no NVS do Meshtastic. Cifra §5.5 adiada. |
| Q4 | Quanto o device desserializa do backup | **Parse light / carry opaque** — só os campos operacionais (canal/gateway/estações/epoch) vão à RAM; o backup completo é **streamado**, nunca inteiro em RAM (§11.2). |
| Q5 | Persistência do cliente ativo | **Arquivo-ponteiro em LittleFS** (`/clientes/active`). **Sem bump de settings ABI.** O canal sintonizado já persiste no `channelFile` do Meshtastic (NVS). |
| Q6 | Onde mora o código | **Novos TUs** (`ServiceBackup`/`ServiceVault`/`ServiceController` + `IProfileStore.h`) — `IrrigationModule.cpp` já tem 2584 linhas; segue o padrão núcleo-puro/glue-fino do repo. |
| Q7 | Escopo da varredura | **Só o cliente ativo** (single-channel, sem reboot no meio da varredura). Reboot-sweep multi-cliente adiado. |
| Q8 | Import no gateway (reprovisionar) | **Adiado.** 8b entrega o export do gateway e o cofre/import no device; o import-no-gateway é follow-on. |

## 3. Compatibilidade e modelo de ameaça

**Aditividade total.** A 8b não muda o wire nem a ABI. O device SERVICO usa mensagens que a 8a já ensinou o parque a aceitar. Nós de firmware anterior à 8a rejeitam o SERVICO (por isso §11.9/8a precede o device em campo) — comportamento esperado, não regressão.

**Autoridade = posse da PSK (§11.1).** O cofre guarda PSKs de vários clientes em claro no LittleFS. Consequência já registrada na spec: quem detém o device detém controle administrativo sobre todas as fazendas cujos perfis ele contém. A 8b não adiciona PKI, consentimento nem escopo — decisão explícita de projeto. A cifra por senha do export (§5.5) fica adiada; até lá, arquivos exportados (USB/SD) são legíveis e devem ser tratados como segredo pelo operador.

**Segurança física do re-tune.** Trocar de cliente reescreve o `channelFile` primário e reinicia. O teto fail-safe de 120 min (§4.2) permanece; nós ao alcance do canal novo continuam sob suas próprias regras locais. Nenhum comando é emitido antes do settle time (§10) pós-boot.

**Airtime.** O emissor da sonda e os comandos do device passam pelo `RateLimiter` existente; a varredura é single-channel e limitada à janela de escuta (§10).

## 4. Formato do backup (§5.5) e envelope multi-cliente

JSON hand-rolled (o repo **não tem ArduinoJson**; reusa o writer/reader mínimo já usado em `IrrigationWebApi`). Legível, tolerante a versão.

**Envelope de import/export (multi-cliente):**
```json
{ "fmt": "irrig-vault", "version": 1, "clients": [ <clientBackup>, … ] }
```

**`clientBackup` (= backup §5.5 de uma fazenda):**
```json
{
  "id": "fazenda-sp-01",
  "nome": "Sitio Boa Vista",
  "canal":   { "nome": "bv-irrig", "psk_b64": "…", "modem_preset": "LONG_FAST" },
  "gateway": "!a1b2c3d4",
  "estacoes": [ { "no": "!e5f6a7b8", "nome": "Pasto Norte", "lat": -22.1, "lon": -47.6 } ],
  "snapshot_epoch": { "!e5f6a7b8": 17 },
  "seq":            { "!e5f6a7b8": 4213 },
  "config": {
    "zonas": [ … ], "programas": [ … ], "intertravamentos": [ … ],
    "grupos": [ … ], "sensorNames": [ … ]
  }
}
```

- **Light** (parseado à RAM pelo device): `id, nome, canal, gateway, estacoes, snapshot_epoch`. `seq` mora em arquivo separado no cofre (ver §5.2).
- **Opaque** (carregado, nunca parseado pelo device, só pelo produtor gateway e por um futuro import-no-gateway): `config{…}`.
- **Merge por `id`** no import: sobrescreve perfis homônimos, preserva os demais; `--replace` zera o cofre antes. Validação integral em área temporária antes de qualquer substituição (§11.7).

## 5. Componentes

### 5.1 `ServiceBackup` — codec puro (§5.5)

Nativo-testável, sem I/O — recebe dados como parâmetro, nunca lê tabela/arquivo direto. Funções:
- `buildClientBackup(snapshots/accessors das tabelas) → JSON` — recebe os dados já colhidos pelo glue do gateway (§5.4); não toca I/O.
- `parseEnvelope(json, visitor)` — itera clientes em streaming; não materializa o envelope inteiro.
- `extractLight(clientJson) → LightProfile` — só campos operacionais.
- `mergeEnvelope(indexAtual, entrada, replace) → plano de merge` — por id.
- `validate(json) → bool` — estrutura + campos obrigatórios + base64 da PSK.

### 5.2 `ServiceVault` — cofre sobre `IProfileStore`

Lógica pura sobre uma interface injetável; a impl LittleFS é glue, a impl RAM-map é o duplê de teste.

```
struct IProfileStore {
  virtual bool   listIds(callback) = 0;             // itera ids do índice
  virtual bool   readProfile(id, streamSink) = 0;   // streaming do <id>.json
  virtual bool   writeProfileAtomic(id, bytes) = 0; // staging + rename
  virtual bool   removeProfile(id) = 0;
  virtual bool   readSeq(id, out) / writeSeq(id, in) = 0;  // <id>.seq
  virtual bool   getActive(out) / setActive(id) = 0;       // /clientes/active
};
```

Layout on-disk (§11.2):
```
/clientes/index.json   → [{ id, nome, canal, gateway, arquivo }]   (metadados leves; NÃO os backups)
/clientes/<id>.json    → clientBackup completo (opaque)
/clientes/<id>.seq     → contadores de sequência por nó (arquivo separado — grava a cada comando, poupa flash)
/clientes/active       → id do cliente ativo
/log/servico.jsonl     → log append-only local (uptime + timestamp adotado do gateway; sem RTC)
```

Impl LittleFS (glue): reusa o padrão `stagedWrite`/`stagedRead` (staging tmp + `renameFile`, guardado por `#ifdef FSCom`) já presente em `IrrigationModule.cpp`. Leitura de perfil é **streaming** via handle FSCom; o índice leve é o único carregado inteiro. Sem FSCom (nativo puro) o glue degrada para no-op — os testes usam o duplê RAM.

Operações puras do cofre: `importEnvelope`, `listClients()→LightProfile[]`, `select(id)`, `seqFor(node)/bumpSeq(node)`, `recordPending(node, epoch)`, `exportEnvelope()`.

### 5.3 `ServiceController` — runtime (glue; sub-peças puras extraídas)

Instanciado por `IrrigationModule` quando `role == SERVICO`. Peças puras isoladas para teste nativo; o resto é glue de rádio/NVS/reboot (compila no nativo, validado em banca).

- **Re-tune (§11.3):** `selectClient(id)` → `channelFromProfile(light)` (helper **puro**: monta `meshtastic_ChannelSettings` a partir de nome/psk_b64/preset) → escreve no `channelFile` primário (`Channels::setChannel`) → `vault.setActive(id)` → **reboot**. No boot, aguarda settle time (§10) com a fila de TX limpa antes de qualquer emissão.
- **Varredura (§11.4, cliente ativo):** emite `PING_SURVEY` PROBE (broadcast) → janela de escuta (§10) → coleta REPLYs (role, epoch, vbat, fwVersion, lat/lon; SNR/RSSI do rádio no receptor) numa **tabela de resultado pura**; funde no registro leve de estações do cliente ativo.
- **Config r/w 2 rotas (§11.6):** decisão de rota **pura** — `VIA_GATEWAY` (envia `SET_CONFIG` ao `gateway` do cliente, que incrementa o epoch e propaga §5.4) vs `DIRECT` (gateway inacessível: escreve na estação com `epoch = atual+1`, `FLAG_FROM_SERVICE`, e `vault.recordPending(node, epoch)`; o gateway, ao reencontrar, adota pela regra do maior epoch — já pronta na 8a, `GatewayTables::adoptConfig`). `GET_CONFIG` sempre direto, não altera epoch; a config lida pode ser exportada no formato de backup (clonar estação).
- **RESYNC requester (§11.5):** predicado **puro** `needsResync(hasSeq)`; sem contador para (cliente,nó) → emite `RESYNC_SEQ` REQUEST → no REPLY faz `seq = lastSeq+1`, persiste `<id>.seq`, prossegue. Responder já entregue na 8a.

Todo comando de serviço é auditado com `AuditOrigin::SERVICO` (enum já existe).

### 5.4 Export do gateway (item adiado, agora IN)

No papel `GATEWAY`, `buildFullBackup()` serializa as tabelas vivas (`StationRegistry`, `ZoneTable`, programas do `ProgramScheduler`, `InterlockTable`, `HydraulicGroupTable`, `SensorNameTable`) + a PSK do canal primário (`Channels::getPrimary().psk` → base64) + epochs num `clientBackup` §5.5. O **builder** é puro (nativo-testável, sobre snapshots das tabelas); a leitura de PSK e a exposição são glue. Exposto como endpoint CI-only `GET /api/irrigation/export` + botão no painel (fecha o adiado da 6b/7b). Produz exatamente o que o cofre do device importa.

### 5.5 Import/export vias (§11.7)

- **Engine** (puro, `ServiceBackup` + `ServiceVault.importEnvelope`): valida em área temporária → merge por id → aplica.
- **USB serial** (glue, bancada): comandos `EXPORT` (despeja envelope) / `IMPORT` (lê envelope do serial). Caminho de provisionamento da 8b.
- **SD-card + portal** (`GET /export` / `POST /import`) → **8c** (dependem do portal).

## 6. Arquitetura / arquivos tocados

| Arquivo | Mudança |
|---|---|
| `ServiceBackup.h/.cpp` (novo) | codec §5.5 puro + envelope + merge + validate |
| `ServiceVault.h/.cpp` (novo) | cofre sobre `IProfileStore`; impl LittleFS (glue) + lógica pura |
| `IProfileStore.h` (novo) | interface do store + duplê RAM p/ teste |
| `ServiceController.h/.cpp` (novo) | runtime SERVICO; sub-peças puras (channelFromProfile, rota, scan-table, needsResync) |
| `IrrigationModule.h/.cpp` | instancia `ServiceController` se `role==SERVICO`; tick SERVICO; serial `EXPORT`/`IMPORT`; wiring dos emits (scan/config/resync); hook do export do gateway |
| `IrrigationGateway.*` ou `IrrigationWebApi/WebEndpoints` | `buildFullBackup` (builder puro) + endpoint CI-only `GET /api/irrigation/export` + botão do painel |
| `data/irrigacao/` | botão de export no painel do gateway (o portal do device é 8c) |

Sem ABI de settings, sem tipo de wire novo, sem bump de VERSION.

## 7. Testes

**Restrição do repo:** nenhum teste nativo instancia `IrrigationModule` (depende do stack mesh). Segue o padrão: **lógica pura nativa-testada; wiring do handler validado por compilação nativa + banca de hardware**. `ServiceController` compila no nativo; o glue LittleFS degrada a no-op sem FSCom (testes usam o duplê RAM do `IProfileStore`).

Novas suítes nativas (TDD no plano):
- **`test_service_backup`** — round-trip `buildClientBackup`→`parseEnvelope`; `extractLight`; `mergeEnvelope` (merge por id, `--replace`); `validate` (rejeita PSK base64 inválida, campo obrigatório ausente, envelope truncado).
- **`test_service_vault`** — sobre o duplê RAM: import de envelope, `listClients`, `select`+`getActive`, `seqFor`/`bumpSeq` (arquivo `.seq` separado), `recordPending`, `exportEnvelope` (round-trip com o import).
- **`test_service_controller`** — peças puras: `channelFromProfile` (nome/psk/preset → ChannelSettings), decisão de rota (`VIA_GATEWAY` vs `DIRECT` com `epoch+1`), tabela de resultado da varredura, `needsResync`.

Glue (compila no nativo + banca, sem TDD nativo): re-tune real (`channelFile`+reboot+settle), emissão de PROBE/REQUEST/SET_CONFIG, serial EXPORT/IMPORT, endpoint de export do gateway.

Suite nativa completa deve passar de **56 → ~59 GREEN** (3 suítes novas; sem mudança nas existentes salvo o builder de export tocando `test_irrigation_webapi`). Confirmação via Docker (memória `windows-native-test-docker`).

## 8. Critérios de aceite

1. Device provisionado por envelope multi-cliente (import USB) lista os clientes; `select` re-tuna o rádio e reinicia no canal do cliente.
2. Gateway exporta um backup §5.5 completo (PSK + todas as tabelas); ele faz round-trip pelo cofre do device (import→export idêntico, módulo merge).
3. Comando do device marcado `FLAG_FROM_SERVICE`: rota via-gateway incrementa o epoch pelo gateway; rota direta escreve `epoch+1` e registra pendência; o gateway adota depois pela regra do maior epoch.
4. Device com contador desconhecido emite `RESYNC_SEQ`, retoma em `last_seq+1`, comando aceito pela estação.
5. Varredura do cliente ativo emite PROBE e registra respondentes (id/role/epoch/vbat/fw/coordenadas/SNR).
6. ABI de settings v5 e protocolo VERSION 1 intactos; nós antigos não quebram.
7. Suíte nativa ~59/59 GREEN.

## 9. Fora de escopo (adiado, com destino)

- Import no gateway (reprovisionar substituto a partir de backup) — follow-on.
- Portal do device §11.8 (Clientes/Rede/Log), e as vias SD-card + portal do import/export — **8c**.
- Cifra por senha da PSK no export (§5.5) — follow-on.
- Varredura multi-cliente com reboot-sweep (cursor persistido) — follow-on; beacon de survey §8.5 — **8d**.

**Banca 2+ nós EXIGIDA antes de campo** — re-tune, varredura por rádio e as 2 rotas de config entre binários não são nativo-testáveis.
