# Design — Fase 9: Painel de estação (detalhe · edição · remoção · pulso)

- **Data:** 2026-08-01
- **Branch:** sistema-irrigacao
- **UI modelo:** `C:\Users\Jefferson\Downloads\Irrigacao Mobile.dc.html` (sheet de estação, `stationSheet`)
- **Arquivos front:** `data/irrigacao/app.js` (`renderStations`), `data/irrigacao/style.css`, `data/irrigacao/mock.js`, `data/irrigacao/test_mock.js`
- **Arquivos firmware:** `src/modules/irrigation/IrrigationSettings.h/.cpp`, `StationMonitor.h/.cpp`, `IrrigationWebApi.h/.cpp`, `IrrigationWebEndpoints.cpp`, `IrrigationModule.h/.cpp`, `StationTelemetryCache.h`

## Problema

A aba **Estações** do painel (`renderStations`, `app.js:181`) é um card estático: nome, "há X · Y V", um único chip de sync. Não é clicável e não expõe a telemetria/config que o firmware já coleta. A UI modelo tem card clicável → bottom-sheet com detalhe completo, pills de estado, mini-log, edição de config, remoção e teste de pulso.

## Domínio — 3 fontes de dado (todas já existem no firmware)

| Fonte | Struct | Conteúdo | Natureza |
|-------|--------|----------|----------|
| Telemetria viva | `StationTelemetry` (`StationTelemetryCache.h`) | vbat, vpanel, snr, **rssi**, reboots, flags, configEpoch | read-only (do heartbeat) |
| Registry gateway | `StationEntry` (`GatewayTables.h`) | name, `desiredEpoch`, `blob`(v6), lat/lon (×1e-5), silencioAlertaMin | mutável gateway-side |
| Blob desejado do nó | `IrrigationSettings` v6 (`IrrigationSettings.h`) | hbMinutes, **vbatAvisoCentiV**, **vbatCriticaCentiV**, latE7/lonE7, pins… | editável → push §5.4 |

`GET /api/irrigation/stations` (`IrrigationWebEndpoints.cpp:113`) já monta `StationView` de telemetria + registry. `sync` = `computeSync(desiredEpoch, configEpoch reportado, silent)`.

## Decisões de produto (travadas com o usuário)

1. **Dois limiares de bateria** configuráveis por estação → **bump ABI v6** no blob.
2. **Teste de pulso** com **escolha da saída** no sheet (enumera saídas configuradas do nó).
3. **RSSI** adicionado ao `/stations` (já vem no `Heartbeat.rssi`, hoje descartado no `StationView`).

## Escopo

- **Dentro:** card clicável + pills + bottom-sheet (view/edit) + mini-log; ABI v6 (2 limiares); `calculateLevel` por-estação; 3 endpoints novos (`stations/config`, `stations/delete`, `stations/pulse`); rssi no `/stations`; mock atualizado.
- **Fora (YAGNI):** edição de pinos/sensores/interlocks pelo sheet (já há telas próprias); histórico gráfico; mapa de coordenadas; OTA.

## Ordem de execução: A → B → C → D

Cada fase é independente, compila e passa no native suite isoladamente.

---

### Fase A — UI interativa read-only (só front; zero firmware)

**Dado:** consome `/stations` (já entrega node, name, sync, secsSinceHeard, vbat, vpanel, snrQuarterDb, rebootCount, flags, lat, lon) + `/audit` (mini-log filtrado por node). *(RSSI só aparece após Fase B.)*

**`renderStations` reescrito:**
- Card clicável (`data-node`), chevron `›`.
- **Pills** (derivadas, sem backend novo):
  - Sync: `sincronizada`/`pendente`/`inalcancavel` (campo `sync`).
  - Integridade: `flags & 1`→Tamper, `&2`→Modo seguro, `&4`→Hibernando (bits `HbFlags`).
  - Bateria (heurística com as constantes atuais até Fase B): `vbat<1180`→crítica, `<1220`→aviso.
  - Enlace: `secsSinceHeard`>silêncio→Silenciosa; `snrQuarterDb/4 < ~3 dB`→degradando.
- **Bottom-sheet detalhe (view):** header (nome, `!hex`, último contato), sync label colorida, grid 2×2 (Bateria V, Painel V, SNR dB [/RSSI após B], Reboots), coordenadas (lat/lon ÷1e5), heartbeat + limiares (placeholder até B: "—"), mini-log (`/audit` filtrado por `node`), botão Editar (abre modo edit, só stub em A) + Fechar.
- Overlay: reusar/estender CSS de modal. Fechar por clique no backdrop e botão.

**Sync/estado:** `POLLED.stations` já re-renderiza; ao reabrir manter sheet aberto do node atual (guardar `openStationNode` em estado do módulo).

**Testes A:** `test_mock.js` — asserts de que `renderStations` gera card clicável + pills corretas por fixture; mini-log filtra por node.

---

### Fase B — Editar config + re-push SET_CONFIG (firmware + front)

**ABI v6** (`IrrigationSettings.h`):
- Append no FIM do struct (após `localInterlocks`, offset 176) para preservar todos os offsets v5:
  ```
  // v6 (Fase 9): limiares de bateria por-estação (centi-volt)
  uint16_t vbatAvisoCentiV = 1220;   // default = StationMonitor::AVISO_CV atual
  uint16_t vbatCriticaCentiV = 1180; // default = CRITICO_CV atual
  ```
- `version = 6`; `sizeof == 180` (176 + 4); novo `static_assert(offsetof(..., vbatAvisoCentiV) == 176)`.
- `IRRIGATION_SETTINGS_V5_SIZE = 176`; `migrateIrrigationSettings`: v5→v6 preenche defaults (1220/1180); manter aceite de v1..v5.
- Comentário de layout no topo atualizado.

**`StationMonitor` por-estação** (`StationMonitor.h/.cpp`):
- `calculateLevel(vbat)` → `calculateLevel(vbat, avisoCv, criticaCv)`; hibernação segue `HIBER_CV` (constante) por ora.
- `onHeartbeat(...)` recebe os limiares da estação. Origem: gateway lê `StationEntry.blob` da estação (via registry). Quando `desiredEpoch==0` (sem blob) → fallback constantes `AVISO_CV`/`CRITICO_CV`. Chamador em `IrrigationModule` resolve os limiares e passa.
- Manter constantes como defaults/fallback.

**Gateway** (`IrrigationModule.h/.cpp`):
- `bool gwApplyStationConfig(uint32_t node, uint16_t hbMinutes, uint16_t vbatAvisoCentiV, uint16_t vbatCriticaCentiV, int32_t latE7, int32_t lonE7)`:
  - `mutableByNode(node)`; se `desiredEpoch==0` → rejeita (blob ainda não adotado, §5.4).
  - Deserializa blob → seta campos → serializa de volta ao `entry->blob`; atualiza `entry->lat/lon` (×1e-5 a partir de latE7/lonE7); `desiredEpoch += 1`.
  - Persiste registry; dispara reconciliação (mesma rota de `gwApplyZoneUpsert`) → push `SET_CONFIG`.
  - Espelha padrão existente (`IrrigationModule.cpp:2459`/`2270`/`3075`).

**Endpoint** `POST /api/irrigation/stations/config` (`IrrigationWebEndpoints.cpp` + parser em `IrrigationWebApi`):
- Body: `{node, hbMinutes, vbatAvisoV, vbatCriticaV, lat, lon}`.
- Parser + validação (`parseStationConfig`): hbMinutes 1..1440; aviso>critica; aviso/critica 800..1500 cV; lat/lon range. Erros → `{"errors":[...]}` 400 (padrão `sendParseErrors`).
- OK → `gwApplyStationConfig`; resposta = `/stations` atualizado.

**RSSI no `/stations`:**
- `StationTelemetry` já tem snr; adicionar `int16_t rssiDbm` (preencher no ingest do heartbeat, `Heartbeat.rssi`).
- `StationView.rssiDbm` + `buildStations` emite `keyNum("rssiDbm", ...)`.

**Front:**
- Sheet view: mostrar SNR/RSSI reais; heartbeat + limiares reais (do `/stations` — **precisa** expor hbMinutes/aviso/critica no `StationView`+`/stations`; adicionar `keyNum` p/ os três).
- Sheet edit: inputs heartbeat, aviso V, crítica V, lat, lon → `POST /stations/config`; on-success reload; estação vira "pendente" até ACK (pills já refletem).

**Testes B:** parser (`parseStationConfig` válido/ inválido), migração v5→v6 (defaults, offsets), `calculateLevel` com limiares custom + fallback, round-trip serialize/deserialize v6.

---

### Fase C — Remover estação (firmware + front)

**Gateway:** `bool gwRemoveStation(uint32_t node, ...)`:
- Conta zonas com `z->node == node` na `ZoneTable`. Se >0 → rejeita, devolve lista de nomes (dependências).
- Se 0 → `stations.removeByNode(node)` + limpa telemetria/monitor/epochCooldown do node + persiste.

**Endpoint** `POST /api/irrigation/stations/delete`:
- Body `{node}`. Deps>0 → `{"errors":["zonas vinculadas: ..."]}` 400. OK → `{"ok":true}`.

**Front:** sheet edit → botão "Remover estação" + confirmação inline + banner de dependências (quando 400 com deps).

**Testes C:** `gwRemoveStation` com/sem zonas vinculadas; endpoint deps.

---

### Fase D — Teste de pulso (firmware + front)

**Saídas do nó:** enumerar do blob (`pinsHbridgeA/B` configurados → válvulas; `pinsGpo` → GPO). Expor no `/stations` um array `outputs:[{tipo,index,label}]` OU derivar no front do blob (blob não exposto ao front hoje → expor `outputs` no `StationView`/`buildStations`).

**Gateway:** `bool gwStationPulse(uint32_t node, uint8_t tipo, uint8_t index, uint16_t durationS)`:
- Valida saída existe no blob; envia `MSG_CMD_VALVULA` (tipo válvula) ou `MSG_CMD_GPO` (tipo gpo) com action=abrir, `durationS=10` fixo (clamp por `blob.maxOpenConfigS` quando >0). Nó emite `EV_TEST_PULSE`.
- Rate-limit/autorização = mesmas regras de comando.

**Endpoint** `POST /api/irrigation/stations/pulse`: body `{node, tipo, index, durationS}`.

**Front:** sheet view → seletor de saída (chips do `outputs`) + "Teste de pulso (10 s)" → estados idle/enviando/aberto/fechado (feedback local temporizado, como o model).

**Testes D:** `gwStationPulse` valida saída inexistente; endpoint parse.

---

## Riscos / notas

- **ABI v6:** único ponto de risco alto. Apêndice no fim + static_asserts pinados + migração testada evitam drift silencioso. Nenhum campo v5 se move.
- **Limiares em dois lugares:** node blob (v6) + consumo gateway em `StationMonitor`. Fonte única = blob desejado da estação; gateway lê dele. Node pode passar a auto-avaliar depois (fora de escopo).
- **Coords em duas escalas:** `StationEntry.lat/lon` (×1e-5, exibição/`/stations`) e blob `latE7/lonE7` (×1e-7, push ao nó). `gwApplyStationConfig` escreve os dois consistentemente.
- **Pulso zone-vs-saída:** `/command` atual é zone-scoped; pulso por saída física é caminho novo (`gwStationPulse`), não reusa `hCommand`.

## Verificação

- `./bin/run-tests.sh` verde após cada fase.
- Front via `mock.js` (adicionar campos rssiDbm, hbMinutes, vbatAviso/CriticaV, outputs às estações mock) — preview offline.
