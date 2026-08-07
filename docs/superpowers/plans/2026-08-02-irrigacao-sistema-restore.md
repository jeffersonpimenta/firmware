# Aba Sistema: Restaurar + limpeza de cards — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "Restaurar" button to the gateway panel Sistema tab that imports a §5.5 backup and reapplies the config tables (zonas, programas, intertravamentos, grupos) while KEEPING the current PSK/channel (no reboot); and remove the two dead info cards (Chave da fazenda, PIN).

**Architecture:** A pure native-testable function reuses `ServiceBackup`'s structural JSON scanner + the existing per-item parsers (`parseZoneUpsert`/`parseProgramUpsert`/`parseInterlockUpsert`/`parseGroupUpsert`) to apply a backup's four config-table arrays into in-memory gateway tables. Thin module glue persists them and station metadata; a CI-only endpoint streams the upload. Frontend swaps two cards for a file-picker Restore card.

**Tech Stack:** C++ (ESP32/Arduino), PlatformIO, Unity native tests, vanilla JS frontend.

## Global Constraints

- Restore is **tables-only (option B)**: applies `zonas`, `programas`, `intertravamentos`, `grupos`. Does **NOT** write PSK/channel, does **NOT** reboot.
- **No forced station config re-push.** The backup carries only station name/coords + 3 config fields (NOT the full 180 B blob), so reconstructing and pushing a station blob would push incomplete config. Restore updates station **metadata** (name/lat/lon) into `StationRegistry` if present; it must NOT bump `desiredEpoch` or push `SET_CONFIG` from restored data. (This is a safety refinement of the spec, which said "re-push"; the backup format cannot safely support it. Station config re-asserts through the normal epoch-reconcile path.)
- No protocol change (VERSION=1), no `IrrigationSettings` ABI change. Reuses existing gateway tables.
- Import parser reuses `ServiceBackup` scanner (`jsonMember`, `jsonForEachArray`, `jsonStr`, `jsonInt`, `validateEnvelope`) and existing per-item parsers — do NOT hand-roll new JSON parsing.
- Endpoint lives in `IrrigationWebEndpoints.cpp` (webserver-guarded, excluded from native), gated `role==GATEWAY`.
- Pure import function is native-tested. `test/native-suite-count` unchanged (cases added to an existing suite).
- Do NOT run `trunk fmt` on Windows host.

## File Structure

- `src/modules/irrigation/IrrigationWebApi.{h,cpp}` — MODIFY: pure `importConfigTablesFromBackup(...)`.
- `test/test_irrigation_webapi/test_main.cpp` — MODIFY: import cases.
- `src/modules/irrigation/IrrigationModule.{h,cpp}` — MODIFY: `gwImportTables` glue.
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — MODIFY: `POST /api/irrigation/import`.
- `data/irrigacao/app.js`, `data/irrigacao/mock.js` — MODIFY: `renderSistema` (remove 2 cards + Restore) + mock.

---

### Task 1: Pure import of the four config tables

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`, `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `ServiceBackup` scanner (`IrrigationService::jsonMember`, `jsonForEachArray`, `Slice`), `parseZoneUpsert`, `parseProgramUpsert`, `parseInterlockUpsert`, `parseGroupUpsert`, and the tables `ZoneTable`/`ProgramScheduler`/`InterlockTable`/`HydraulicGroupTable`.
- Produces:
  ```cpp
  struct ImportCounts { uint8_t zonas=0, programas=0, intertravamentos=0, grupos=0; };
  // Aplica os 4 arrays de tabela do primeiro client do envelope. NÃO toca PSK/estações.
  // false = envelope inválido (validateEnvelope) — tabelas ficam intactas.
  bool importConfigTablesFromBackup(const char *json, size_t len, ZoneTable &zones,
                                    ProgramScheduler &sched, InterlockTable &interlocks,
                                    HydraulicGroupTable &groups, ImportCounts &out, char *err, size_t errCap);
  ```

- [ ] **Step 1: Write failing test**

In `test/test_irrigation_webapi/test_main.cpp` add a test that builds a minimal valid envelope and imports it. (Match the exact section key names emitted by `buildClientBackup` in `ServiceBackup.cpp` / `gwBuildBackup` — verify them before finalizing the literal below; they are the Portuguese keys `zonas`/`programas`/`intertravamentos`/`grupos`.)

```cpp
static void test_import_config_tables_ok()
{
    // Envelope §5.5 mínimo: fmt/version + clients[0] com canal.psk_b64 + 1 zona + 1 grupo.
    const char *env =
      "{\"fmt\":\"irrig-vault\",\"version\":1,\"clients\":[{"
      "\"id\":\"faz1\",\"gateway\":\"!a1b2c3d4\",\"canal\":{\"psk_b64\":\"AAAA\"},"
      "\"zonas\":[{\"id\":3,\"nome\":\"Horta\",\"node\":\"!00000010\",\"tipo\":0,\"index\":0,\"maxMin\":120,\"padraoMin\":20}],"
      "\"programas\":[],\"intertravamentos\":[],"
      "\"grupos\":[{\"id\":1,\"nome\":\"G1\",\"zonas\":[3],\"maxOpen\":1}]"
      "}]}";
    ZoneTable z; ProgramScheduler s; InterlockTable il; HydraulicGroupTable g;
    ImportCounts c; char err[48];
    bool ok = importConfigTablesFromBackup(env, strlen(env), z, s, il, g, c, err, sizeof(err));
    TEST_ASSERT_TRUE_MESSAGE(ok, err);
    TEST_ASSERT_EQUAL_UINT8(1, c.zonas);
    TEST_ASSERT_EQUAL_UINT8(1, c.grupos);
    TEST_ASSERT_NOT_NULL(z.byId(3));
}

static void test_import_rejects_bad_envelope()
{
    const char *bad = "{\"fmt\":\"nope\"}";
    ZoneTable z; ProgramScheduler s; InterlockTable il; HydraulicGroupTable g;
    ImportCounts c; char err[48];
    TEST_ASSERT_FALSE(importConfigTablesFromBackup(bad, strlen(bad), z, s, il, g, c, err, sizeof(err)));
    TEST_ASSERT_EQUAL_size_t(0, z.count()); // tabela intacta
}
```

Register both in `setup()`. Adjust the zona/grupo JSON so it round-trips through `parseZoneUpsert`/`parseGroupUpsert` (inspect those parsers for required keys before finalizing).

- [ ] **Step 2: Run to verify failure**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED — `importConfigTablesFromBackup` undefined.

- [ ] **Step 3: Declare + implement**

Declare `ImportCounts` and `importConfigTablesFromBackup` in `IrrigationWebApi.h` (include `ServiceBackup.h` for the scanner and the table headers as already done for `buildZones` etc.).

Implement in `IrrigationWebApi.cpp` using the scanner + per-item parsers:

```cpp
using IrrigationService::Slice;
namespace {
struct SectionCtx { void *table; uint8_t *count; ParseResult (*apply)(Slice, void *table); };
// helper: iterate one array member and apply each element
bool importArray(const char *client, size_t cn, const char *key,
                 bool (*applyElem)(void *, Slice), void *ctx) {
    Slice arr;
    if (!IrrigationService::jsonMember(client, cn, key, arr)) return true; // ausente = 0 itens
    return IrrigationService::jsonForEachArray(arr, ctx, applyElem);
}
} // namespace

bool importConfigTablesFromBackup(const char *json, size_t len, ZoneTable &zones,
                                  ProgramScheduler &sched, InterlockTable &interlocks,
                                  HydraulicGroupTable &groups, ImportCounts &out, char *err, size_t errCap)
{
    if (!IrrigationService::validateEnvelope(json, len, err, errCap)) return false;
    // Primeiro client do envelope.
    Slice client{};
    struct { Slice *dst; } cctx{&client};
    IrrigationService::envelopeForEachClient(json, len, &cctx, [](void *c, Slice cl) {
        ((decltype(cctx) *)c)->dst->p = cl.p; ((decltype(cctx) *)c)->dst->n = cl.n; return false; // só o 1º
    });
    if (!client.p) { snprintf(err, errCap, "sem client"); return false; }

    // zonas
    struct ZCtx { ZoneTable *t; uint8_t *n; } zc{&zones, &out.zonas};
    importArray(client.p, client.n, "zonas", [](void *v, Slice e) {
        auto *x = (ZCtx *)v; Zone z{};
        if (parseZoneUpsert(e.p, e.n, z).ok && x->t->upsert(z)) (*x->n)++;
        return true;
    }, &zc);
    // programas
    struct PCtx { ProgramScheduler *t; uint8_t *n; } pc{&sched, &out.programas};
    importArray(client.p, client.n, "programas", [](void *v, Slice e) {
        auto *x = (PCtx *)v; Program p{};
        if (parseProgramUpsert(e.p, e.n, p).ok && x->t->upsert(p)) (*x->n)++;
        return true;
    }, &pc);
    // intertravamentos
    struct ICtx { InterlockTable *t; uint8_t *n; } ic{&interlocks, &out.intertravamentos};
    importArray(client.p, client.n, "intertravamentos", [](void *v, Slice e) {
        auto *x = (ICtx *)v; InterlockRule r{};
        if (parseInterlockUpsert(e.p, e.n, r).ok && x->t->upsert(r)) (*x->n)++;
        return true;
    }, &ic);
    // grupos
    struct GCtx { HydraulicGroupTable *t; uint8_t *n; } gc{&groups, &out.grupos};
    importArray(client.p, client.n, "grupos", [](void *v, Slice e) {
        auto *x = (GCtx *)v; HydraulicGroup g{};
        if (parseGroupUpsert(e.p, e.n, g).ok && x->t->upsert(g)) (*x->n)++;
        return true;
    }, &gc);
    return true;
}
```

(Verify exact upsert method names/signatures: `ProgramScheduler::upsert(Program)`, `InterlockTable::upsert(InterlockRule)`, `HydraulicGroupTable::upsert(HydraulicGroup)` — adapt if different. Verify `envelopeForEachClient` exists as declared in ServiceBackup.h. The lambda-to-function-pointer casts require captureless lambdas — the pattern above is captureless with ctx structs.)

- [ ] **Step 4: Run to verify pass**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, all cases (existing + 2 new) pass.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): pure import of backup config tables (native-tested)"
```

---

### Task 2: Gateway glue + import endpoint (CI/bancada-only)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `src/modules/irrigation/IrrigationModule.cpp`, `src/modules/irrigation/IrrigationWebEndpoints.cpp`

**Interfaces:**
- Consumes: `importConfigTablesFromBackup` (Task 1), the gateway tables in `gwState()`, `saveGatewayState()`, `auditEvent`.
- Produces: `bool IrrigationModule::gwImportTables(const char *json, size_t len, char *resp, size_t respCap);` — imports, persists, writes a small JSON result `{ok, zonas, programas, intertravamentos, grupos}` (or an error). Does NOT touch PSK/channel; does NOT reboot; does NOT bump station epochs.

- [ ] **Step 1: Declare + implement glue**

In `IrrigationModule.h` add the declaration. In `IrrigationModule.cpp`:

```cpp
bool IrrigationModule::gwImportTables(const char *json, size_t len, char *resp, size_t respCap)
{
    ImportCounts c; char err[48];
    if (!importConfigTablesFromBackup(json, len, gateway.zones, gateway.scheduler,
                                      gateway.interlocks, gateway.groups, c, err, sizeof(err))) {
        snprintf(resp, respCap, "{\"ok\":false,\"err\":\"%s\"}", err);
        return false;
    }
    saveGatewayState(); // persiste zonas/programas/intertravamentos/grupos (mesmo save já existente)
    auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_ACEITO, 0, AuditResult::ACK, 0);
    snprintf(resp, respCap,
             "{\"ok\":true,\"zonas\":%u,\"programas\":%u,\"intertravamentos\":%u,\"grupos\":%u}",
             c.zonas, c.programas, c.intertravamentos, c.grupos);
    return true;
}
```

(Confirm the real field names on the gateway state struct: `gateway.zones`, `gateway.scheduler`, `gateway.interlocks`, `gateway.groups` — adapt to actual names. Confirm `saveGatewayState()` is the function that serializes all four tables at IrrigationModule.cpp:~1956; if tables persist via separate helpers, call each. Match the actual `auditEvent` signature.)

- [ ] **Step 2: Add the endpoint**

In `IrrigationWebEndpoints.cpp`, add `POST /api/irrigation/import` mirroring the streaming-body pattern of the ServicePortal import (or, if simpler, the body-read pattern of `hZoneUpsert`). Gated `role==GATEWAY`. Read the request body into a buffer (cap ≥ the backup size the gateway can produce; reuse the same cap constant `gwBuildBackup` uses), call `gwImportTables(body, nb, resp, sizeof(resp))`, respond `application/json` with `resp` (HTTP 200 on ok, 400 on `ok:false`). Register the route in both `ContentHandler` registration sites like the other `/api/irrigation/*` POSTs.

- [ ] **Step 3: Verify full native suite + module compile**

```bash
./bin/run-tests.sh
```
Expected: `RESULT: GREEN` (baseline count). Glue compiles in native; endpoint excluded from native.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp src/modules/irrigation/IrrigationWebEndpoints.cpp
git commit -m "feat(irrigation): gateway import endpoint (tables-only, keeps PSK) (CI-only)"
```

---

### Task 3: Frontend — Sistema card cleanup + Restore

**Files:**
- Modify: `data/irrigacao/app.js` (`renderSistema`, app.js:2214), `data/irrigacao/mock.js`

**Interfaces:**
- Consumes: `POST /api/irrigation/import` (Task 2).

- [ ] **Step 1: Rewrite `renderSistema`**

Remove the "Chave da fazenda" (app.js:2222-2225) and "PIN de aplicação" (app.js:2226-2229) cards. Keep the Backup card. Add a "Restaurar" card below Backup:

```js
`<div class="card">
   <div class="sens-hdr"><span class="name">Restaurar</span></div>
   <div class="sub maint-sub">Importa um backup e reaplica as tabelas de configuração (zonas, programas, intertravamentos, grupos). <b>Mantém a chave da rede</b> e não reinicia. As estações não são reconfiguradas (só metadados).</div>
   <input type="file" id="restoreFile" accept=".json,application/json" class="sub">
   <button class="btn solid big syslink" id="doRestore">⬆ Restaurar backup (.json)</button>
   <div class="sub" id="restoreStatus"></div>
 </div>`
```

Wire the button: read the chosen file as text, `POST API + '/import'` with the text body, show the returned counts on success (`{ok, zonas, programas, …}`) or the error on failure. Follow the robust fetch+feedback style of `downloadBackup` (app.js:2188). Disable the button while in-flight.

- [ ] **Step 2: Mock support**

In `data/irrigacao/mock.js`, add a `POST /api/irrigation/import` handler that parses the uploaded envelope's `clients[0]` and echoes counts `{ok:true, zonas:N, …}` (and applies them to `STATE` so the panel reflects the import), matching the existing mock consistency style.

- [ ] **Step 3: Verify (frontend only → native unaffected)**

Confirm `git diff --name-only` shows only `data/irrigacao/*`. Browser visual check with mock is a manual follow-up (uncomment mock include locally; do NOT commit that toggle).

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/app.js data/irrigacao/mock.js
git commit -m "feat(irrigation): painel Sistema — Restaurar + remove cards chave/PIN"
```

---

## Notes for the executor

- Endpoint/glue/frontend are CI/bancada-only; native validates the pure import (Task 1) + module compile (Task 2).
- **Deviation from spec (safety):** station config is NOT re-pushed from the backup (backup lacks the full 180 B blob). Restore updates only the 4 config tables; station metadata restore is optional and must never bump epoch/push SET_CONFIG. Flagged to the user.
- **Banca exigida**: import real via HTTP + persistência não são native-testáveis.
- Match actual `JsonReader`/scanner/table `upsert`/`auditEvent`/persistence APIs in the files you touch; the code blocks show intended shapes — verify names against neighbours before compiling.
- Do NOT run `trunk fmt` on Windows host.
