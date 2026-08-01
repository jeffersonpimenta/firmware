# Fase 9 — Painel de estação Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Aba Estações vira card clicável → bottom-sheet com detalhe/telemetria, edição de config (re-push SET_CONFIG), remoção e teste de pulso.

**Architecture:** Front (`app.js`/`style.css`/`mock.js`) portado da UI modelo. Firmware reusa infra existente: telemetria (`StationTelemetry`), registry (`StationEntry`/`StationRegistry`), push de config §5.4 (`desiredEpoch`+`SET_CONFIG`). ABI do blob do nó sobe a v6 (2 limiares de bateria por-estação).

**Tech Stack:** C++ (Unity native tests via PlatformIO), JS vanilla (mock testado por node), CSS.

## Global Constraints

- Native tests: `./bin/run-tests.sh` (exit 0 GREEN). Suites-alvo: `test_irrigation_config`, `test_irrigation_monitor`, `test_irrigation_gwtables`, `test_irrigation_webapi`.
- Front test: `node test_mock.js` (rodar de `data/irrigacao/`) — só valida consistência de `STATE`.
- Format: `trunk fmt` antes de cada commit.
- ABI: `IrrigationSettings` é ABI on-disk/rádio — apêndice no FIM, offsets v5 imutáveis, static_asserts pinados, `version` bump.
- Endpoints seguem padrão `IrrigationWebEndpoints.cpp` (guard webserver, `sendJson`, `sendParseErrors`).
- Coords: `StationEntry.lat/lon` ×1e-5 (exibição/`/stations`); blob `latE7/lonE7` ×1e-7 (push nó).

---

## FASE A — UI interativa read-only (só front)

### Task A1: mock — audit por-node p/ mini-log

**Files:**
- Modify: `data/irrigacao/mock.js` (STATE.audit / handler `/audit`)
- Test: `data/irrigacao/test_mock.js`

**Interfaces:**
- Produces: `STATE.stations[i]` com `node`; `/audit` retorna entradas com `node` p/ filtro no front.

- [ ] **Step 1: Verificar shape atual de `/audit` no mock** — confirmar se há entradas com `node`. Se ausente, adicionar ≥3 entradas cujo `node` casa com `STATE.stations[0].node`.
- [ ] **Step 2: Add assert em `test_mock.js`**

```js
check(Array.isArray(STATE.audit) && STATE.audit.some((a) => a.node === STATE.stations[0].node),
  'audit deve ter >=1 entrada do node da estacao 0 (mini-log)');
```

- [ ] **Step 3: Rodar** `node test_mock.js` (de `data/irrigacao/`) → `OK: mock ...`
- [ ] **Step 4: Commit** `test(irrigation): mock — audit por-node p/ mini-log da estação`

### Task A2: renderStations — card clicável + pills

**Files:**
- Modify: `data/irrigacao/app.js` (`renderStations` ~181; SYNC_CLASS já existe)
- Modify: `data/irrigacao/style.css`

**Interfaces:**
- Consumes: `/stations` (node, name, sync, secsSinceHeard, vbatCentiV, vpanelCentiV, snrQuarterDb, rebootCount, flags, lat, lon).
- Produces: card com `data-node="<hex>"`, `click` → `openStationSheet(node)` (Task A3).

- [ ] **Step 1:** Reescrever `renderStations` — card por estação com nome, sub (`fmtSince · fmtVolts`), chevron `›`, e pills via helper `stationPills(s)`:

```js
function stationPills(s) {
  const p = [];
  const sync = s.sync || 'inalcancavel';
  const syncMap = { sincronizada: ['green','Sincronizada'], pendente: ['amber','Pendente'], inalcancavel: ['red','Inalcançável'] };
  const sm = syncMap[sync] || syncMap.inalcancavel; p.push([sm[0], sm[1]]);
  const f = num(s.flags);
  if (f & 1) p.push(['red','Tamper']);
  if (f & 2) p.push(['red','Modo seguro']);
  if (f & 4) p.push(['amber','Hibernando']);
  const vb = num(s.vbatCentiV);
  if (vb && vb < 1180) p.push(['red','Bateria crítica']);
  else if (vb && vb < 1220) p.push(['amber','Bateria em aviso']);
  const snr = num(s.snrQuarterDb) / 4;
  if (s.snrQuarterDb != null && snr < 3) p.push(['amber','Enlace degradando']);
  return p.map(([c,t]) => `<span class="chip ${c}">${esc(t)}</span>`).join('');
}
```

Card:
```js
`<div class="card station" data-node="${nodeHex(s.node)}" role="button" tabindex="0">
   <div class="st-main"><div class="name">${name}</div>
     <div class="sub">${fmtSince(s.secsSinceHeard)} · ${fmtVolts(s.vbatCentiV)}</div>
     <div class="chips">${stationPills(s)}</div></div>
   <span class="chev">›</span>
 </div>`
```
Após render, wire: cada `.card.station` → `click`/Enter → `openStationSheet(parseInt(dataset.node,16))`.

- [ ] **Step 2:** CSS `.card.station{display:flex;align-items:center;gap:12px}` `.st-main{flex:1;min-width:0}` `.chev{color:#9aa;font-size:18px}` `.chip.amber{...}` (checar se `amber` já existe; senão add).
- [ ] **Step 3:** Preview browser (mock) — cards clicáveis, pills corretas. (verificação manual; sem DOM test.)
- [ ] **Step 4: Commit** `feat(irrigation): estações — card clicável + pills de estado`

### Task A3: bottom-sheet detalhe (view) + mini-log

**Files:**
- Modify: `data/irrigacao/app.js` (novo `openStationSheet`, `closeStationSheet`, `renderStationSheet`)
- Modify: `data/irrigacao/style.css` (overlay/sheet)

**Interfaces:**
- Consumes: `/stations` (busca por node), `/audit` (filtra por node).
- Produces: overlay DOM `#stSheet`; estado módulo `openStationNode`.

- [ ] **Step 1:** `openStationSheet(node)` guarda `openStationNode=node`, busca dados (`getJson('/stations')`, `getJson('/audit').catch(()=>[])`), monta overlay:
  - Header: nome, `!hex`, "último contato {fmtSince}"; sync label colorida.
  - Grid 2×2: Bateria `fmtVolts(vbat)`, Painel `fmtVolts(vpanel)`, "SNR {snr/4} dB" (RSSI só na Fase B → omitir agora), Reboots.
  - Coordenadas: `lat/1e5, lon/1e5` (ou "—" se 0/0).
  - Heartbeat & limiares: "—" (placeholder até Fase B).
  - Mini-log: `audit.filter(a=>a.node===node)` → linhas (ts, ação); vazio → "Sem eventos".
  - Botões: Editar (stub: `alert`/no-op até Fase B), Fechar.
- [ ] **Step 2:** `closeStationSheet()` remove overlay + `openStationNode=null`. Backdrop click + botão fecham. `stopPropagation` no corpo.
- [ ] **Step 3:** No re-poll de `stations`, se `openStationNode` set, re-render sheet (mantém aberto).
- [ ] **Step 4:** CSS overlay: `.sheet-backdrop{position:fixed;inset:0;background:rgba(0,0,0,.4);display:flex;align-items:flex-end;z-index:100}` `.sheet{width:100%;background:#fff;border-radius:20px 20px 0 0;padding:14px 20px 34px;max-height:88%;overflow:auto}` + grid.
- [ ] **Step 5:** Preview browser — abre/fecha, dados certos, mini-log filtrado.
- [ ] **Step 6: Commit** `feat(irrigation): estações — bottom-sheet de detalhe + mini-log`

---

## FASE B — Editar config + re-push (firmware + front)

### Task B1: ABI v6 — 2 limiares de bateria

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h` (struct, asserts, comentário)
- Modify: `src/modules/irrigation/IrrigationSettings.cpp` (`migrateIrrigationSettings`)
- Test: `test/test_irrigation_config/test_main.cpp`

**Interfaces:**
- Produces: `IrrigationSettings::vbatAvisoCentiV` (uint16, default 1220), `::vbatCriticaCentiV` (uint16, default 1180); `version=6`; `sizeof==180`; `IRRIGATION_SETTINGS_V5_SIZE=176`.

- [ ] **Step 1: Teste falho** — em `test_irrigation_config`, add:

```cpp
static void test_v6_defaults_and_size()
{
    IrrigationSettings s;
    TEST_ASSERT_EQUAL_UINT16(6, s.version);
    TEST_ASSERT_EQUAL_UINT16(1220, s.vbatAvisoCentiV);
    TEST_ASSERT_EQUAL_UINT16(1180, s.vbatCriticaCentiV);
    TEST_ASSERT_EQUAL_UINT(180, sizeof(IrrigationSettings));
}
static void test_migrate_v5_to_v6_fills_defaults()
{
    IrrigationSettings v5; v5.version = 5;            // struct atual como se fosse v5
    uint8_t raw[176]; memcpy(raw, &v5, 176);          // só o prefixo v5
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, 176, out));
    TEST_ASSERT_EQUAL_UINT16(1220, out.vbatAvisoCentiV);
    TEST_ASSERT_EQUAL_UINT16(1180, out.vbatCriticaCentiV);
}
```
Registrar no `RUN_TEST` do arquivo.

- [ ] **Step 2: Rodar** `./bin/run-tests.sh -f test_irrigation_config` → RED (campos/tamanho não existem).
- [ ] **Step 3: Implementar** em `IrrigationSettings.h`: `version = 6`; após `localInterlocks[...]` add:

```cpp
  // v6 (Fase 9): limiares de bateria por-estação (centi-volt), avaliados gateway-side.
  uint16_t vbatAvisoCentiV = 1220;
  uint16_t vbatCriticaCentiV = 1180;
```
Asserts: `static_assert(offsetof(IrrigationSettings, vbatAvisoCentiV) == 176, "ABI v6");` `static_assert(sizeof(IrrigationSettings) == 180, ...)`; add `static constexpr size_t IRRIGATION_SETTINGS_V5_SIZE = 176;`. Atualizar comentário de layout (v6, total 180).
Em `.cpp` `migrateIrrigationSettings`: aceitar `n>=176` como v5 (copia 176, seta version=6, defaults 1220/1180); manter ramos v1..v4; `n>=180` → v6 direto.

- [ ] **Step 4: Rodar** `./bin/run-tests.sh -f test_irrigation_config` → GREEN.
- [ ] **Step 5:** `trunk fmt` + **Commit** `feat(irrigation): ABI v6 — limiares de bateria por-estação no blob`

### Task B2: StationMonitor — calculateLevel por-estação

**Files:**
- Modify: `src/modules/irrigation/StationMonitor.h/.cpp`
- Modify: chamador em `src/modules/irrigation/IrrigationModule.cpp` (onde `onHeartbeat` é chamado)
- Test: `test/test_irrigation_monitor/test_main.cpp`

**Interfaces:**
- Produces: `uint8_t calculateLevel(uint16_t vbat, uint16_t avisoCv, uint16_t criticaCv) const;` `onHeartbeat(node, vbat, rebootCount, nowMs, out, uint16_t avisoCv, uint16_t criticaCv)` (params novos com default = `AVISO_CV`/`CRITICO_CV`).

- [ ] **Step 1: Teste falho:**

```cpp
static void test_calculateLevel_custom_thresholds()
{
    StationMonitor m;
    // aviso=1300, critica=1250 → 1280 cai em aviso, 1240 em critico
    TEST_ASSERT_EQUAL_UINT8(1, m.calculateLevel(1280, 1300, 1250));
    TEST_ASSERT_EQUAL_UINT8(2, m.calculateLevel(1240, 1300, 1250));
    TEST_ASSERT_EQUAL_UINT8(0, m.calculateLevel(1350, 1300, 1250));
}
```
- [ ] **Step 2: Rodar** `-f test_irrigation_monitor` → RED.
- [ ] **Step 3: Implementar** `calculateLevel(vbat, aviso, critica)` (hibernação segue `HIBER_CV`; histerese `HYST_CV` mantida relativa aos limiares passados). `onHeartbeat` usa os params (default constantes). Chamador em `IrrigationModule` resolve limiares: `const StationEntry* e = gateway.stations.byNode(node); IrrigationSettings cfg; se e && e->desiredEpoch && migrate(e->blob,180,cfg) → usa cfg.vbatAvisoCentiV/Critica; senão constantes`.
- [ ] **Step 4: Rodar** `-f test_irrigation_monitor` → GREEN.
- [ ] **Step 5:** `trunk fmt` + **Commit** `feat(irrigation): StationMonitor — limiares de bateria por-estação`

### Task B3: gwApplyStationConfig + parser + endpoint

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h/.cpp` (`gwApplyStationConfig`)
- Modify: `src/modules/irrigation/IrrigationWebApi.h/.cpp` (`StationConfigReq`, `parseStationConfig`)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (`hStationsConfig` + registro)
- Test: `test/test_irrigation_webapi/test_main.cpp`, `test/test_irrigation_gwtables/test_main.cpp`

**Interfaces:**
- Produces:
  - `struct StationConfigReq { uint32_t node; uint16_t hbMinutes; uint16_t vbatAvisoCentiV; uint16_t vbatCriticaCentiV; int32_t latE7; int32_t lonE7; };`
  - `ParseResult parseStationConfig(const char* body, size_t n, StationConfigReq& out);`
  - `bool IrrigationModule::gwApplyStationConfig(const StationConfigReq& r);`
  - Endpoint `POST /api/irrigation/stations/config`.

- [ ] **Step 1: Teste parser (webapi):**

```cpp
static void test_parseStationConfig_ok()
{
    const char *b = "{\"node\":123,\"hbMinutes\":15,\"vbatAvisoV\":12.0,\"vbatCriticaV\":11.5,\"lat\":-22.9068,\"lon\":-47.0616}";
    StationConfigReq r; ParseResult pr = parseStationConfig(b, strlen(b), r);
    TEST_ASSERT_TRUE(pr.ok);
    TEST_ASSERT_EQUAL_UINT16(15, r.hbMinutes);
    TEST_ASSERT_EQUAL_UINT16(1200, r.vbatAvisoCentiV);
    TEST_ASSERT_EQUAL_UINT16(1150, r.vbatCriticaCentiV);
}
static void test_parseStationConfig_aviso_le_critica_rejeita()
{
    const char *b = "{\"node\":1,\"hbMinutes\":10,\"vbatAvisoV\":11.0,\"vbatCriticaV\":11.5,\"lat\":0,\"lon\":0}";
    StationConfigReq r; ParseResult pr = parseStationConfig(b, strlen(b), r);
    TEST_ASSERT_FALSE(pr.ok);
}
```
- [ ] **Step 2: Rodar** `-f test_irrigation_webapi` → RED.
- [ ] **Step 3: Implementar** `parseStationConfig` (usar util JSON existente do módulo; validar hbMinutes 1..1440; aviso/critica 800..1500 cV; aviso>critica; lat/lon → latE7/lonE7). `StationConfigReq` no header.
- [ ] **Step 4: Rodar** `-f test_irrigation_webapi` → GREEN.
- [ ] **Step 5: Teste gwtables** — `gwApplyStationConfig` muta blob + bump epoch:

```cpp
static void test_gwApplyStationConfig_bumps_epoch()
{
    StationRegistry reg; StationEntry e{}; e.node = 7; e.desiredEpoch = 3;
    IrrigationSettings s; uint8_t blob[180]; memcpy(blob,&s,180); memcpy(e.blob, blob, 180);
    reg.upsert(e);
    // aplica via helper puro testável (extrair core p/ StationRegistry se preciso), ou testar efeito:
    StationEntry *m = reg.mutableByNode(7);
    // simula core de gwApplyStationConfig:
    IrrigationSettings cfg; migrateIrrigationSettings(m->blob, 180, cfg);
    cfg.hbMinutes = 20; cfg.vbatAvisoCentiV = 1250;
    // serialize back...
    memcpy(m->blob, &cfg, 180); m->desiredEpoch += 1;
    TEST_ASSERT_EQUAL_UINT32(4, m->desiredEpoch);
    IrrigationSettings chk; migrateIrrigationSettings(m->blob, 180, chk);
    TEST_ASSERT_EQUAL_UINT16(20, chk.hbMinutes);
}
```
(Se a lógica de mutação for extraível como função pura em GatewayTables, testá-la diretamente; senão manter o teste do efeito no registry.)
- [ ] **Step 6:** Implementar `gwApplyStationConfig` em `IrrigationModule.cpp` (espelha `gwApplyZoneUpsert`: `mutableByNode`; rejeita `desiredEpoch==0`; migra blob→edita hbMinutes/aviso/critica/latE7/lonE7→serializa; `entry->lat=latE7/100, entry->lon=lonE7/100`; `desiredEpoch+=1`; persiste; reconcilia push). Endpoint `hStationsConfig` + registro em `registerIrrigationHandlers`.
- [ ] **Step 7: Rodar** `-f test_irrigation_gwtables` + `-f test_irrigation_webapi` → GREEN.
- [ ] **Step 8:** `trunk fmt` + **Commit** `feat(irrigation): endpoint POST /stations/config + re-push SET_CONFIG`

### Task B4: rssi + hb/limiares no /stations

**Files:**
- Modify: `src/modules/irrigation/StationTelemetryCache.h` (`int16_t rssiDbm`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (ingest heartbeat → grava `rssiDbm`)
- Modify: `src/modules/irrigation/IrrigationWebApi.h/.cpp` (`StationView.rssiDbm/hbMinutes/vbatAvisoCentiV/vbatCriticaCentiV`; `buildStations` emite)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (`hStations` preenche hb/limiares do blob)
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Produces: `/stations[i]` ganha `rssiDbm, hbMinutes, vbatAvisoCentiV, vbatCriticaCentiV`.

- [ ] **Step 1: Teste buildStations:**

```cpp
static void test_buildStations_emits_rssi_and_config()
{
    StationView v{}; v.node=1; v.name="X"; v.rssiDbm=-72; v.hbMinutes=15; v.vbatAvisoCentiV=1220; v.vbatCriticaCentiV=1180;
    char buf[512]; TEST_ASSERT_GREATER_THAN(0, buildStations(&v,1,buf,sizeof(buf)));
    TEST_ASSERT_TRUE(contains(buf, "\"rssiDbm\":-72"));
    TEST_ASSERT_TRUE(contains(buf, "\"hbMinutes\":15"));
    TEST_ASSERT_TRUE(contains(buf, "\"vbatCriticaCentiV\":1180"));
}
```
- [ ] **Step 2: Rodar** `-f test_irrigation_webapi` → RED.
- [ ] **Step 3: Implementar** campos no `StationView`, `keyNum` no `buildStations`, `rssiDbm` no `StationTelemetry` + ingest, `hStations` lê hb/limiares do blob desejado (migrate; fallback defaults quando `desiredEpoch==0`).
- [ ] **Step 4: Rodar** `-f test_irrigation_webapi` → GREEN.
- [ ] **Step 5:** `trunk fmt` + **Commit** `feat(irrigation): /stations expõe rssi + heartbeat + limiares`

### Task B5: front — sheet mostra SNR/RSSI + hb/limiares; modo edit

**Files:**
- Modify: `data/irrigacao/app.js` (`renderStationSheet` view + edit), `data/irrigacao/mock.js` (campos novos), `data/irrigacao/test_mock.js`
- Modify: `data/irrigacao/style.css`

- [ ] **Step 1:** mock: add `rssiDbm, hbMinutes, vbatAvisoV/CriticaV` (ou centiV) às estações; assert em test_mock.js (`stations[0].hbMinutes` numérico). Rodar `node test_mock.js`.
- [ ] **Step 2:** view sheet: grid mostra "SNR {snr} dB / RSSI {rssi} dBm"; bloco heartbeat "A cada {hbMinutes} min", "Aviso {aviso} V · crítica {critica} V".
- [ ] **Step 3:** botão Editar → modo edit: inputs heartbeat(min), aviso(V), crítica(V), lat, lon; Salvar → `postJson('/stations/config', {node,hbMinutes,vbatAvisoV,vbatCriticaV,lat,lon})`; on-ok reload+view; on-erro mostra `errors`. Cancelar volta view.
- [ ] **Step 4:** Preview browser — editar salva; estação vira "pendente".
- [ ] **Step 5: Commit** `feat(irrigation): sheet de estação — SNR/RSSI, heartbeat e edição de config`

---

## FASE C — Remover estação

### Task C1: gwRemoveStation + endpoint

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h/.cpp` (`gwRemoveStation`)
- Modify: `src/modules/irrigation/IrrigationWebApi.h/.cpp` (parse `{node}`; reuso se já houver)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (`hStationsDelete` + registro)
- Test: `test/test_irrigation_gwtables/test_main.cpp`

**Interfaces:**
- Produces: `int IrrigationModule::gwCountZonesForNode(uint32_t node) const;` `bool IrrigationModule::gwRemoveStation(uint32_t node);` (false se deps>0); endpoint `POST /api/irrigation/stations/delete`.

- [ ] **Step 1: Teste** — remover com zona vinculada falha, sem zona ok:

```cpp
static void test_removeByNode_present_then_absent()
{
    StationRegistry reg; StationEntry e{}; e.node=9; reg.upsert(e);
    TEST_ASSERT_NOT_NULL(reg.byNode(9));
    TEST_ASSERT_TRUE(reg.removeByNode(9));
    TEST_ASSERT_NULL(reg.byNode(9));
}
```
(Dependência de zona testada no nível do módulo se houver harness; senão asserção do registry + revisão manual do guard no endpoint.)
- [ ] **Step 2: Rodar** `-f test_irrigation_gwtables` → RED (se assert novo) / baseline.
- [ ] **Step 3: Implementar** `gwCountZonesForNode` (varre `ZoneTable`), `gwRemoveStation` (deps>0→false; senão `removeByNode` + limpa telemetry/monitor/epochCooldown + persiste). Endpoint: deps>0 → `{"errors":["zonas vinculadas: <nomes>"]}` 400; senão `{"ok":true}`.
- [ ] **Step 4: Rodar** `-f test_irrigation_gwtables` → GREEN.
- [ ] **Step 5:** `trunk fmt` + **Commit** `feat(irrigation): endpoint POST /stations/delete (trava zonas vinculadas)`

### Task C2: front — remover no sheet edit

**Files:** Modify `data/irrigacao/app.js`, `data/irrigacao/style.css`

- [ ] **Step 1:** No modo edit: botão "Remover estação" → confirmação inline → `postJson('/stations/delete',{node})`; on-ok fecha sheet + reload lista; on-erro (deps) mostra banner "zonas vinculadas: …".
- [ ] **Step 2:** Preview browser — remove ok; com zona vinculada mostra banner.
- [ ] **Step 3: Commit** `feat(irrigation): sheet de estação — remoção com trava de dependências`

---

## FASE D — Teste de pulso

### Task D1: outputs no /stations + gwStationPulse + endpoint

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h/.cpp` (`StationView.outputs` serializado; ou array derivado)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (`hStations` popula outputs do blob; `hStationsPulse` + registro)
- Modify: `src/modules/irrigation/IrrigationModule.h/.cpp` (`gwStationPulse`)
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Produces: `/stations[i].outputs = [{tipo,index,label}]`; `bool gwStationPulse(uint32_t node, uint8_t tipo, uint8_t index, uint16_t durationS);` endpoint `POST /api/irrigation/stations/pulse`.

- [ ] **Step 1: Teste** — outputs derivados do blob (pins configurados viram saída):

```cpp
static void test_buildStations_emits_outputs()
{
    StationView v{}; v.node=1; v.name="X";
    v.outputCount=1; v.outputs[0]={/*tipo*/0,/*index*/0};
    char buf[512]; buildStations(&v,1,buf,sizeof(buf));
    TEST_ASSERT_TRUE(contains(buf, "\"outputs\""));
}
```
(Ajustar à representação real escolhida no header.)
- [ ] **Step 2: Rodar** `-f test_irrigation_webapi` → RED.
- [ ] **Step 3: Implementar** enumeração de saídas (pinsHbridgeA/B configurados → válvula index i; pinsGpo → gpo index i) em `hStations`; `buildStations` emite `outputs`. `gwStationPulse` valida saída no blob → `MSG_CMD_VALVULA`/`MSG_CMD_GPO` action=abrir durS=10 (clamp `maxOpenConfigS`), regras de autorização/rate-limit. Endpoint `hStationsPulse` (parse `{node,tipo,index,durationS}`).
- [ ] **Step 4: Rodar** `-f test_irrigation_webapi` → GREEN.
- [ ] **Step 5:** `trunk fmt` + **Commit** `feat(irrigation): endpoint POST /stations/pulse + outputs no /stations`

### Task D2: front — seletor de saída + teste de pulso

**Files:** Modify `data/irrigacao/app.js`, `data/irrigacao/style.css`, `data/irrigacao/mock.js`, `data/irrigacao/test_mock.js`

- [ ] **Step 1:** mock: estações ganham `outputs`; assert em test_mock.js. Rodar `node test_mock.js`.
- [ ] **Step 2:** view sheet: chips de saída (de `outputs`) + "Teste de pulso (10 s)" → `postJson('/stations/pulse',{node,tipo,index,durationS:10})`; estados idle/enviando/aberto/fechado (feedback temporizado local).
- [ ] **Step 3:** Preview browser.
- [ ] **Step 4: Commit** `feat(irrigation): sheet de estação — teste de pulso por saída`

---

## Verificação final

- [ ] `./bin/run-tests.sh` → GREEN (todas suites).
- [ ] `node test_mock.js` (de `data/irrigacao/`) → OK.
- [ ] Preview browser: fluxo completo lista→detalhe→editar→pendente→remover→pulso.
- [ ] `superpowers:finishing-a-development-branch`.

## Self-review notes

- Cobertura do spec: A(card/pills/sheet/minilog)=A1-A3; B(v6/limiares/config/rssi/edit)=B1-B5; C(delete)=C1-C2; D(pulso/outputs)=D1-D2. ✓
- Front sem DOM test é limitação do harness (`test_mock.js` só valida STATE) — verificação de render é manual/browser, explícito em cada task.
- Nomes consistentes: `gwApplyStationConfig`, `gwRemoveStation`, `gwStationPulse`, `StationConfigReq`, `parseStationConfig`, `openStationSheet`, `stationPills`.
