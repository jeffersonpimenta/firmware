# Task 4 Report — Mirror endpoints + gateway glue (CI-only)

## Status: DONE

---

## Glue methods implemented (`IrrigationModule.h` + `IrrigationModule.cpp`)

Four public methods added after `gwBuildAlerts` in the `.h` public section and implemented in the `.cpp` after `gwBuildAlerts` (line ~2713):

### `gwSetMirrorEnabled(bool enabled)`
- Calls `gateway.mirror.setEnabled(enabled)` (confirmed in `MirrorMode.h`: `void setEnabled(bool e)`)
- Persists with `saveGatewayState()` (private method, line 1944 in .cpp — already serializes mirror via `gateway.mirror.serialize()` at line 1968)
- Audits: `AuditOrigin::PAINEL`, `AuditAction::CONFIG_EPOCH`, target=0, `AuditResult::OK`

### `gwApplyMirrorMapping(int8_t input, uint8_t zoneId, bool invertido, bool habilitado, char *err, size_t errCap) → bool`
- Validates zone via `gateway.zones.byId(zoneId)` (`ZoneTable::byId` confirmed in `GatewayTables.h`)
- Enforces one-zone-per-port: iterates `gateway.zones.zoneAt(i)` for `i < ZoneTable::MAX` (=24), clears `fonteInput` on any other zone holding `input`
- Upserts the target zone with `fonteInput=input`, `fonteEnabled=habilitado?1:0`
- Sets/clears bit `input` of `settings.digitalInActiveLow` per `invertido` (field confirmed in `IrrigationSettings.h` line 44)
- Persists: `saveIrrigationSettings(settings)` (free function, `IrrigationSettings.h` line 130) then `saveGatewayState()`
- Audits: `AuditOrigin::PAINEL`, `AuditAction::CONFIG_EPOCH`, target=zoneId, `AuditResult::OK`

### `gwDeleteMirrorMapping(int8_t input) → bool`
- Looks up zone via `gateway.zones.byFonte(input)` (`ZoneTable::byFonte` in `GatewayTables.h` line 25)
- Returns false if none; sets `fonteInput = -1` on found zone, upserts, calls `saveGatewayState()`
- Audits same pattern

### `gwBuildMirror(char *buf, size_t cap) → size_t`
- Reads live GPIO state via `gateway.mirror.inputActive(i)` for i in [0, MIN(4, MAX_DIGITAL_IN)) (`inputActive` confirmed in `MirrorMode.h` line 27)
- Calls `IrrigationWeb::buildMirror(buf, cap, gateway.mirror.enabled(), gateway.zones, settings.digitalInActiveLow, live)` (signature confirmed in `IrrigationWebApi.h` lines 267-268)

---

## Audit action selection

`AuditAction` (in `AuditLog.h`) has no `CMD_ACEITO` value. The brief said "else `CMD_ACEITO`" as a fallback but that enum value does not exist. Used `AuditAction::CONFIG_EPOCH` — the existing action used for configuration changes (`portalProvision`, `handleSetConfig`). Semantically correct: mirror enable/mapping operations are config mutations, not valve open/close commands.

---

## 4 Endpoints (`IrrigationWebEndpoints.cpp`)

Added between the levels block and `hExport`. All guarded by `gwReady()` (checks `irrigationModule && irrigationModule->gwIsGateway()` — the file's helper at line 26).

| Path | Method | Handler | Body |
|---|---|---|---|
| `/api/irrigation/mirror` | GET | `hMirror` | none |
| `/api/irrigation/mirror` | POST | `hMirrorToggle` | `{enabled:bool}` |
| `/api/irrigation/mirror/mapping` | POST | `hMirrorMapping` | `{input,zoneId,invertido?,habilitado?}` |
| `/api/irrigation/mirror/mapping/delete` | POST | `hMirrorMappingDelete` | `{input:0..3}` |

### Plumbing matched exactly from neighbours:
- `gwReady()` guard — same as every handler in this file
- `readBody(req, body, sizeof(body))` idiom — same as `hZonesPost`, `hInterlocksPost`, etc.
- `sendJson(res, buf)` / `res->setStatusCode(N)` — same everywhere
- `sendParseErrors(res, pr)` for parse failures — same as zone/program/group handlers
- Error 400 with `JsonWriter` inline for apply failures — same as `hGroupsPost`, `hLevelsPost`
- Response = `gwBuildMirror(buf, sizeof(buf))` — mirrors the `buildZones`/`buildGroups` response pattern

### `hMirrorMappingDelete` — input-only parse
`parseMirrorMapping` requires both `input` and `zoneId` (1..255), so it cannot parse `{input}` alone. Used `JsonReader rd(body, nb)` directly (same class used internally by all parsers; available via `using namespace IrrigationWeb;` at line 20 of the file). Validates `input` in [0,3], returns 400 if out of range or if no zone is associated.

---

## Registration

`ContentHandler.cpp` lines 122 and 150 call `registerIrrigationHandlers(secureServer)` and `registerIrrigationHandlers(insecureServer)` — both pass through the same single function in `IrrigationWebEndpoints.cpp`. Added 4 `registerNode` calls before the `hExport` registration block, satisfying both sites.

---

## Persistence cross-check

- `saveGatewayState()` (line 1944): already persists mirror at lines 1964-1970. Called for all mirror state mutations.
- `saveIrrigationSettings(settings)` (free function): called in `gwApplyMirrorMapping` because `digitalInActiveLow` lives in `settings`, not in `gateway`. Exact pattern used in `handleSetConfig` and `activateSettings`.

---

## Concerns

**Minor (non-blocking):**
1. `AuditAction::CONFIG_EPOCH` is used for mirror config mutations. Semantically it was for epoch bumps — if a dedicated `MIRROR_CONFIG` action is added later, these calls would need updating. No ABI impact on existing records.
2. `gwBuildMirror` calls `gateway.mirror.inputActive(i)` which reflects pós-debounce GPIO state — correct for UI display as the brief specifies "reads live GPIO into a bool[4]".
3. `gwApplyMirrorMapping` reads `*zc` (pointer into zone table) before the "clear other zones" loop. The loop upserts other zones but not `zc`'s slot yet, so the pointer remains valid. The copy `z = *zc` is made before `upsert(z)`. No dangling pointer risk.

---

## Code-review fixes (applied after initial Task 4 commit)

Three findings confirmed valid and applied:

### Finding 1 — byFonte clear replaces zoneAt loop (`gwApplyMirrorMapping`)

**Problem:** The one-zone-per-port clear used `for (size_t i = 0; i < ZoneTable::MAX; i++) { zoneAt(i) ... }`. `zoneAt(i)` is an *occupancy-rank* accessor (returns the i-th occupied zone, `nullptr` if i >= count()), not a slot iterator over all MAX=24 slots. Iterating up to MAX instead of count() was semantically wrong (over-iteration beyond occupied slots, getting `nullptr` for indices >= count).

**Fix:** Replaced with `byFonte(input)` — the same single-call lookup already used in `gwDeleteMirrorMapping` and the mirror apply path at line ~3128. The `byFonte`-based clear is placed before copying the target zone:
```cpp
const Zone *other = gateway.zones.byFonte(input);
if (other && other->id != zoneId) {
    Zone upd = *other; upd.fonteInput = -1; gateway.zones.upsert(upd);
}
```
This correctly handles the "reassign" case (same port, different target zone) and is a no-op when the port is unassigned or already owned by the target zone.

### Finding 2 — ESPELHO audit action (all 3 mirror calls)

**Problem:** All three mirror glue methods (`gwSetMirrorEnabled`, `gwApplyMirrorMapping`, `gwDeleteMirrorMapping`) used `AuditAction::CONFIG_EPOCH`, which is the action emitted by `handleSetConfig` for mesh config-epoch bumps — misleading for a safety-relevant panel bypass mode toggle/mapping.

**Fix (AuditLog.h):** Appended `ESPELHO` after `CMD_REJEITADO` (now = 15). Enum is append-only ABI; no existing values reordered. Inline comment documents the new value's meaning (panel mirror bypass, safety-relevant).

**Fix (IrrigationModule.cpp):** Changed all 3 `auditEvent(AuditOrigin::PAINEL, AuditAction::CONFIG_EPOCH, ...)` calls to `AuditAction::ESPELHO`.

**Fix (data/irrigacao/app.js):** Appended `'Espelho'` to `ACOES_LABEL` (now 16 elements, index 0..15). Updated the `// AuditAction:` comment on line ~1100 to include `,ESPELHO=15`. Index alignment verified: CMD_REJEITADO=14 was previously the last element (index 14 of a 15-element array); ESPELHO=15 is now index 15 of a 16-element array.

### Finding 3 — Capture id before upsert (`gwDeleteMirrorMapping`)

**Problem:** The audit call `auditEvent(..., z->id, ...)` read through pointer `z` *after* `gateway.zones.upsert(upd)` — the upsert may mutate the slot that `z` points into (it updates in-place by id), making the dereference potentially stale.

**Fix:** Added `uint8_t savedId = z->id;` before `upd.fonteInput = -1` and the upsert; changed the audit call to use `savedId`.

Note: `gwApplyMirrorMapping` uses the `zoneId` *parameter* in its audit call (not a pointer dereference), so it was not affected.

### Verification checklist
- Enum append-only: `ESPELHO` is the last value in `AuditAction`, after `CMD_REJEITADO`. No existing values reordered. ✓
- ACOES_LABEL alignment: 16 elements (indices 0–15), matching enum values 0–15. ✓
- `byFonte` clear: placed before the target zone copy/modify; correctly skips if port is unassigned or already owned by target zone. ✓
- All 3 mirror audit calls updated to `AuditAction::ESPELHO`. ✓
- `savedId` captured before upsert in `gwDeleteMirrorMapping`. ✓
