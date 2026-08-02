# Modo Espelhamento UI — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the gateway panel a "Modo Espelhamento" screen (aba "Mais") to enable/disable mirror mode and see live input→zone state, faithfully reproducing the mockup — making the already-built `MirrorMode` reachable in production for the first time.

**Architecture:** Reuse existing firmware: global enable = `gateway.mirror.setEnabled/enabled` (add an endpoint); association porta→zona = `Zone.fonteInput`; polarity = gateway `settings.digitalInActiveLow` bit. Add ONE new per-zone field `fonteEnabled` (pause an association without deleting it) to `ZoneTable` (gateway-only flash table, migrated). Pure JSON helpers are native-tested; endpoints + frontend are CI/bancada-only.

**Tech Stack:** C++ (ESP32/Arduino), PlatformIO, Unity native tests, vanilla JS frontend (`data/irrigacao/`).

## Global Constraints

- No protocol change (VERSION=1). No change to `IrrigationSettings` ABI (v6, 180 B).
- The ONLY ABI change is `ZoneTable` on-flash (gateway-only): entry 28→29 bytes, new MAGIC `IZN2` (0x495A4E32), with backward-compatible read of old `IZN1`/28-byte files defaulting `fonteEnabled=1`.
- Mirror drive + scheduler-suppression must honor BOTH `mirror.enabled()` AND per-zone `fonteEnabled`. A disabled association behaves as if not mirrored (scheduler controls the zone).
- Endpoints live in `IrrigationWebEndpoints.cpp` (webserver-guarded, excluded from native via `variants/native/portduino.ini`), gated `role==GATEWAY`.
- Pure helpers go in `IrrigationWebApi.{h,cpp}` and ARE native-tested in `test_irrigation_webapi`.
- Frontend reproduces the mockup screen `C:\Users\Jefferson\Downloads\Irrigacao Mobile.dc.html` lines 815–970 (list + edit) using the existing tokens/patterns in `data/irrigacao/style.css` + `app.js`.
- Run native suite in Docker (see project memory `native-test-docker-cp-workaround`); `test/native-suite-count` unchanged (no new suite — cases added to existing suites).
- Do NOT run `trunk fmt` on Windows host.

## File Structure

- `src/modules/irrigation/GatewayTables.h` — MODIFY: `Zone` gains `uint8_t fonteEnabled`.
- `src/modules/irrigation/GatewayTables.cpp` — MODIFY: `ZoneTable::serialize/deserialize` v2 + migration.
- `src/modules/irrigation/MirrorMode.h/.cpp` — MODIFY: `mirrorOwnsZoneOutput` gains `fonteEnabled` param.
- `src/modules/irrigation/IrrigationModule.cpp` — MODIFY: mirror drive loop honors `fonteEnabled`; new glue methods; comment fix.
- `src/modules/irrigation/IrrigationModule.h` — MODIFY: declare glue methods.
- `src/modules/irrigation/IrrigationWebApi.{h,cpp}` — MODIFY: `parseMirrorToggle`, `parseMirrorMapping`, `buildMirror`.
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — MODIFY: 4 endpoints + registration.
- `test/test_irrigation_gwtables/test_main.cpp` — MODIFY: Zone v2 round-trip + v1 migration.
- `test/test_irrigation_webapi/test_main.cpp` — MODIFY: mirror helper cases.
- `data/irrigacao/app.js`, `data/irrigacao/mock.js` — MODIFY: `renderEspelhamento` + Mais wiring + mock.

---

### Task 1: `Zone.fonteEnabled` + ZoneTable v2 migration

**Files:**
- Modify: `src/modules/irrigation/GatewayTables.h` (Zone struct), `src/modules/irrigation/GatewayTables.cpp` (ZoneTable serialize/deserialize)
- Test: `test/test_irrigation_gwtables/test_main.cpp`

**Interfaces:**
- Produces: `Zone::fonteEnabled` (uint8_t, default 1). `ZoneTable` serializes 29-byte entries under MAGIC `0x495A4E32`; deserializes both new (29B) and legacy (28B, `fonteEnabled=1`).

- [ ] **Step 1: Write failing tests**

In `test/test_irrigation_gwtables/test_main.cpp` add:

```cpp
static void test_zone_fonteEnabled_roundtrip()
{
    ZoneTable t;
    Zone z{}; z.id = 5; z.node = 0x1234; z.fonteInput = 2; z.fonteEnabled = 0;
    strncpy(z.name, "Horta", sizeof(z.name) - 1);
    TEST_ASSERT_TRUE(t.upsert(z));
    uint8_t buf[800];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    ZoneTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    const Zone *r = t2.byId(5);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT8(2, r->fonteInput);
    TEST_ASSERT_EQUAL_UINT8(0, r->fonteEnabled); // pausada preservada
}

static void test_zone_legacy_v1_migrates_enabled()
{
    // Constrói um blob legado IZN1 de 28 bytes/entrada (1 zona com fonteInput=1).
    uint8_t buf[6 + 28] = {0};
    uint32_t magic = 0x495A4E31; memcpy(buf, &magic, 4); buf[4] = 1; buf[5] = 1;
    size_t off = 6;
    buf[off + 0] = 7;                       // id
    memcpy(buf + off + 1, "Z", 1);          // name
    uint32_t node = 0xABCD; memcpy(buf + off + 17, &node, 4);
    buf[off + 21] = 0; buf[off + 22] = 0;   // tipo, index
    uint16_t mm = 120; memcpy(buf + off + 23, &mm, 2);
    uint16_t pm = 20;  memcpy(buf + off + 25, &pm, 2);
    buf[off + 27] = 1;                       // fonteInput
    ZoneTable t;
    TEST_ASSERT_TRUE(t.deserialize(buf, sizeof(buf)));
    const Zone *r = t.byId(7);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT8(1, r->fonteInput);
    TEST_ASSERT_EQUAL_UINT8(1, r->fonteEnabled); // migração default = habilitada
}
```

Register both in the suite's `setup()` with `RUN_TEST(...)`.

- [ ] **Step 2: Run to verify failure**

Run: `./bin/run-tests.sh -f test_irrigation_gwtables`
Expected: RED — `fonteEnabled` not a member / assertions fail.

- [ ] **Step 3: Add the field**

In `GatewayTables.h`, inside `struct Zone`, after `int8_t fonteInput = -1;`:

```cpp
    uint8_t fonteEnabled = 1; // 1 = associação de espelho ativa; 0 = pausada (fonteInput preservado)
```

- [ ] **Step 4: Implement v2 serialize + dual-version deserialize**

In `GatewayTables.cpp`: change `ZONE_ENTRY` to 29 and add the legacy constant + magics:

```cpp
static constexpr size_t ZONE_ENTRY = 29;          // v2: +1 byte fonteEnabled em off+28
static constexpr size_t ZONE_ENTRY_V1 = 28;       // legado
static constexpr uint32_t ZONE_MAGIC_V1 = 0x495A4E31; // "IZN1"
```

Set `ZoneTable::MAGIC` (in `GatewayTables.h`) to `0x495A4E32` ("IZN2").

In `serialize`, after writing `buf[off + 27] = (uint8_t)z.fonteInput;` add:

```cpp
        buf[off + 28] = z.fonteEnabled;
```

Replace `deserialize` body with dual-version parsing:

```cpp
bool ZoneTable::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : zones)
        s = Zone{};
    uint8_t cnt;
    bool v2 = checkHeader(buf, n, MAGIC, ZONE_ENTRY, MAX, cnt);
    bool v1 = false;
    if (!v2)
        v1 = checkHeader(buf, n, ZONE_MAGIC_V1, ZONE_ENTRY_V1, MAX, cnt);
    if (!v2 && !v1)
        return false;
    const size_t stride = v2 ? ZONE_ENTRY : ZONE_ENTRY_V1;
    size_t off = 6;
    for (uint8_t i = 0; i < cnt; i++, off += stride) {
        Zone z;
        z.id = buf[off];
        memcpy(z.name, buf + off + 1, 16);
        z.name[15] = '\0';
        memcpy(&z.node, buf + off + 17, 4);
        z.tipo = buf[off + 21];
        z.index = buf[off + 22];
        memcpy(&z.maxMin, buf + off + 23, 2);
        memcpy(&z.padraoMin, buf + off + 25, 2);
        z.fonteInput = (int8_t)buf[off + 27];
        z.fonteEnabled = v2 ? buf[off + 28] : 1; // legado → habilitada
        zones[i] = z;
    }
    return true;
}
```

- [ ] **Step 5: Fix the stale buffer comment**

In `IrrigationModule.cpp` near line 1865, update the comment `zones: ...24*32 = 774 → 800` to `24*29 = 696 → 800` (buffer stays 800; verify the zones buffer declaration is ≥ 702).

- [ ] **Step 6: Run tests to verify pass**

Run: `./bin/run-tests.sh -f test_irrigation_gwtables`
Expected: FILTERED, all cases pass incl. the two new ones.

- [ ] **Step 7: Commit**

```bash
git add src/modules/irrigation/GatewayTables.h src/modules/irrigation/GatewayTables.cpp test/test_irrigation_gwtables/test_main.cpp src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): Zone.fonteEnabled + ZoneTable v2 migration"
```

---

### Task 2: Mirror gating honors `fonteEnabled`

**Files:**
- Modify: `src/modules/irrigation/MirrorMode.h`, `src/modules/irrigation/MirrorMode.cpp`, `src/modules/irrigation/IrrigationModule.cpp`
- Test: `test/test_irrigation_mirror/test_main.cpp`

**Interfaces:**
- Consumes: `Zone::fonteEnabled` (Task 1).
- Produces: `bool mirrorOwnsZoneOutput(const MirrorMode &m, int8_t fonteInput, bool fonteEnabled)`.

- [ ] **Step 1: Write failing test**

In `test/test_irrigation_mirror/test_main.cpp` add:

```cpp
static void test_mirror_owns_respects_fonteEnabled()
{
    MirrorMode m;
    m.setEnabled(true);
    uint8_t bmp = 0b0001;         // entrada 0 ativa
    m.update(bmp, 1000);          // marca input 0 ativo (após debounce, se necessário chamar 2x)
    m.update(bmp, 1000 + MirrorMode::DEBOUNCE_MS + 1);
    // habilitada: possui a saída
    TEST_ASSERT_TRUE(mirrorOwnsZoneOutput(m, 0, true));
    // pausada: NÃO possui (scheduler controla)
    TEST_ASSERT_FALSE(mirrorOwnsZoneOutput(m, 0, false));
}
```

Register in `setup()`.

- [ ] **Step 2: Run to verify failure**

Run: `./bin/run-tests.sh -f test_irrigation_mirror`
Expected: RED — `mirrorOwnsZoneOutput` takes 2 args, not 3.

- [ ] **Step 3: Update the helper**

In `MirrorMode.h`, change the declaration:

```cpp
bool mirrorOwnsZoneOutput(const MirrorMode &m, int8_t fonteInput, bool fonteEnabled);
```

In `MirrorMode.cpp`, update the definition:

```cpp
bool mirrorOwnsZoneOutput(const MirrorMode &m, int8_t fonteInput, bool fonteEnabled)
{
    return m.enabled() && fonteEnabled && fonteInput >= 0 && m.inputActive((uint8_t)fonteInput);
}
```

- [ ] **Step 4: Update callers in IrrigationModule.cpp**

At the two scheduler-suppression sites (~lines 2958 and 2981), change
`mirrorOwnsZoneOutput(gateway.mirror, z->fonteInput)` to
`mirrorOwnsZoneOutput(gateway.mirror, z->fonteInput, z->fonteEnabled)`.

In the mirror drive loop (~line 3058), after `const Zone *z = gateway.zones.byFonte(ma.input);` and the null check, add:

```cpp
        if (!z->fonteEnabled)
            continue; // associação pausada: espelho não comanda; scheduler controla
```

- [ ] **Step 5: Run tests to verify pass**

Run: `./bin/run-tests.sh -f test_irrigation_mirror`
Expected: FILTERED, cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/MirrorMode.h src/modules/irrigation/MirrorMode.cpp src/modules/irrigation/IrrigationModule.cpp test/test_irrigation_mirror/test_main.cpp
git commit -m "feat(irrigation): mirror honors per-association fonteEnabled"
```

---

### Task 3: Pure web helpers (parse toggle/mapping, build status)

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`, `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `ZoneTable`, `Zone` (Task 1), `MirrorMode`.
- Produces:
  - `ParseResult parseMirrorToggle(const char *json, size_t len, bool &enabled);`
  - `ParseResult parseMirrorMapping(const char *json, size_t len, int8_t &input, uint8_t &zoneId, bool &invertido, bool &habilitado);`
  - `size_t buildMirror(char *buf, size_t cap, bool enabled, const ZoneTable &zones, uint8_t digitalInActiveLow, const bool liveActive[4]);`

- [ ] **Step 1: Write failing tests**

In `test/test_irrigation_webapi/test_main.cpp` add (follow the existing `parse*`/`build*` test idioms in that file — use `JsonReader`/`ParseResult` conventions already present):

```cpp
static void test_parseMirrorToggle_ok()
{
    bool en = false;
    const char *j = "{\"enabled\":true}";
    ParseResult r = parseMirrorToggle(j, strlen(j), en);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_TRUE(en);
}

static void test_parseMirrorMapping_validates_input_range()
{
    int8_t in; uint8_t zid; bool inv, hab;
    const char *bad = "{\"input\":5,\"zoneId\":1,\"invertido\":false,\"habilitado\":true}";
    ParseResult r = parseMirrorMapping(bad, strlen(bad), in, zid, inv, hab);
    TEST_ASSERT_FALSE(r.ok); // input fora de 0..3
}

static void test_parseMirrorMapping_ok()
{
    int8_t in; uint8_t zid; bool inv, hab;
    const char *j = "{\"input\":2,\"zoneId\":7,\"invertido\":true,\"habilitado\":false}";
    ParseResult r = parseMirrorMapping(j, strlen(j), in, zid, inv, hab);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT8(2, in);
    TEST_ASSERT_EQUAL_UINT8(7, zid);
    TEST_ASSERT_TRUE(inv);
    TEST_ASSERT_FALSE(hab);
}

static void test_buildMirror_shape()
{
    ZoneTable z;
    Zone a{}; a.id = 1; a.node = 0x10; a.fonteInput = 0; a.fonteEnabled = 1;
    strncpy(a.name, "Horta", sizeof(a.name) - 1); z.upsert(a);
    bool live[4] = {true, false, false, false};
    char buf[1024];
    size_t n = buildMirror(buf, sizeof(buf), true, z, 0x01 /*in0 activeLow*/, live);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"enabled\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"zoneId\":1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"invertido\":true"));  // in0 activeLow
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active\":true"));      // live[0]
}
```

Register all four in `setup()`.

- [ ] **Step 2: Run to verify failure**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED — functions undefined.

- [ ] **Step 3: Declare in IrrigationWebApi.h**

Add near the other mirror-adjacent declarations:

```cpp
ParseResult parseMirrorToggle(const char *json, size_t len, bool &enabled);
ParseResult parseMirrorMapping(const char *json, size_t len, int8_t &input, uint8_t &zoneId,
                               bool &invertido, bool &habilitado);
size_t buildMirror(char *buf, size_t cap, bool enabled, const ZoneTable &zones,
                   uint8_t digitalInActiveLow, const bool liveActive[4]);
```

- [ ] **Step 4: Implement in IrrigationWebApi.cpp**

Use the existing `JsonReader` (flat reader) and `JsonWriter` helpers already used by `parseZoneUpsert`/`buildZones` in this file. Implementation:

```cpp
ParseResult parseMirrorToggle(const char *json, size_t len, bool &enabled)
{
    ParseResult r; JsonReader rd(json, len);
    if (!rd.getBool("enabled", enabled)) return r.fail("falta 'enabled'");
    return r; // ok
}

ParseResult parseMirrorMapping(const char *json, size_t len, int8_t &input, uint8_t &zoneId,
                               bool &invertido, bool &habilitado)
{
    ParseResult r; JsonReader rd(json, len);
    int in = -1, zid = 0;
    if (!rd.getInt("input", in) || in < 0 || in > 3) return r.fail("input fora de 0..3");
    if (!rd.getInt("zoneId", zid) || zid <= 0 || zid > 255) return r.fail("zoneId inválido");
    bool inv = false, hab = true;
    rd.getBool("invertido", inv);   // opcional (default false)
    rd.getBool("habilitado", hab);  // opcional (default true)
    input = (int8_t)in; zoneId = (uint8_t)zid; invertido = inv; habilitado = hab;
    return r;
}

size_t buildMirror(char *buf, size_t cap, bool enabled, const ZoneTable &zones,
                   uint8_t digitalInActiveLow, const bool liveActive[4])
{
    JsonWriter w(buf, cap);
    w.beginObj();
    w.keyBool("enabled", enabled);
    w.key("ports"); w.beginArr();
    for (int i = 0; i < 4; i++) {
        const Zone *z = zones.byFonte((int8_t)i);
        w.beginObj();
        w.keyNum("i", i);
        w.keyBool("active", liveActive[i]);
        w.keyBool("invertido", (digitalInActiveLow >> i) & 1);
        if (z) {
            w.keyNum("zoneId", z->id);
            w.keyStr("zoneName", z->name);
            w.keyBool("habilitado", z->fonteEnabled != 0);
            w.keyBool("driving", enabled && z->fonteEnabled && liveActive[i]);
        } else {
            w.keyNum("zoneId", 0);
        }
        w.endObj();
    }
    w.endArr();
    w.endObj();
    return w.size();
}
```

(Adapt method names to the actual `JsonReader`/`JsonWriter` API in this file — check `parseZoneUpsert` and `buildZones` for the exact getters/writers, e.g. `getInt`, `getBool`, `keyNum`, `keyStr`, `keyBool`, `beginArr`. If a `getBool` does not exist, parse `"true"` via the existing int/string getter used elsewhere.)

- [ ] **Step 5: Run tests to verify pass**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, all cases (existing + 4 new) pass.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): pure mirror web helpers (toggle/mapping/status)"
```

---

### Task 4: Endpoints + module glue (CI/bancada-only)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `src/modules/irrigation/IrrigationModule.cpp`, `src/modules/irrigation/IrrigationWebEndpoints.cpp`

**Interfaces:**
- Consumes: helpers from Task 3, `Zone.fonteEnabled` (Task 1), existing `saveGatewayState`/zones persistence (IrrigationModule.cpp:1956), `saveIrrigationSettings`.
- Produces (IrrigationModule public methods):
  - `void gwSetMirrorEnabled(bool enabled);`
  - `bool gwApplyMirrorMapping(int8_t input, uint8_t zoneId, bool invertido, bool habilitado, char *err, size_t errCap);`
  - `bool gwDeleteMirrorMapping(int8_t input);`
  - `size_t gwBuildMirror(char *buf, size_t cap);` (reads live GPIO into a bool[4] and calls `buildMirror`)

- [ ] **Step 1: Declare glue methods**

In `IrrigationModule.h` (gateway section, near `gwApplyGroupUpsert`), add the four declarations above.

- [ ] **Step 2: Implement glue in IrrigationModule.cpp**

```cpp
void IrrigationModule::gwSetMirrorEnabled(bool enabled)
{
    gateway.mirror.setEnabled(enabled);
    saveGatewayState(); // persiste o flag do mirror (mesmo save que já grava mirror)
    auditEvent(AuditOrigin::PAINEL,
               enabled ? AuditAction::CMD_ACEITO : AuditAction::CMD_ACEITO, 0, AuditResult::ACK, 0);
}

bool IrrigationModule::gwApplyMirrorMapping(int8_t input, uint8_t zoneId, bool invertido,
                                            bool habilitado, char *err, size_t errCap)
{
    const Zone *zc = gateway.zones.byId(zoneId);
    if (!zc) { snprintf(err, errCap, "zona %u inexistente", zoneId); return false; }
    // Limpa a porta em qualquer outra zona (1 zona por porta).
    for (size_t i = 0; i < ZoneTable::MAX; i++) {
        const Zone *zi = gateway.zones.zoneAt(i);
        if (zi && zi->id != zoneId && zi->fonteInput == input) {
            Zone upd = *zi; upd.fonteInput = -1; gateway.zones.upsert(upd);
        }
    }
    Zone z = *zc;
    z.fonteInput = input;
    z.fonteEnabled = habilitado ? 1 : 0;
    if (!gateway.zones.upsert(z)) { snprintf(err, errCap, "tabela de zonas cheia"); return false; }
    // Polaridade: bit `input` de digitalInActiveLow nos settings do gateway.
    if (invertido) settings.digitalInActiveLow |= (uint8_t)(1u << input);
    else settings.digitalInActiveLow &= (uint8_t)~(1u << input);
    saveIrrigationSettings(settings);
    saveGatewayState();
    auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_ACEITO, zoneId, AuditResult::ACK, 0);
    return true;
}

bool IrrigationModule::gwDeleteMirrorMapping(int8_t input)
{
    const Zone *z = gateway.zones.byFonte(input);
    if (!z) return false;
    Zone upd = *z; upd.fonteInput = -1; gateway.zones.upsert(upd);
    saveGatewayState();
    auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_ACEITO, z->id, AuditResult::ACK, 0);
    return true;
}

size_t IrrigationModule::gwBuildMirror(char *buf, size_t cap)
{
    bool live[4] = {false, false, false, false};
    for (uint8_t i = 0; i < 4 && i < IrrigationSettings::MAX_DIGITAL_IN; i++)
        live[i] = gateway.mirror.inputActive(i);
    return buildMirror(buf, cap, gateway.mirror.enabled(), gateway.zones,
                       settings.digitalInActiveLow, live);
}
```

Notes: reuse the actual persistence function name in this file (the one that serializes zones at line 1956 — likely `saveGatewayState()`); if zones/mirror are saved by separate helpers, call those. Use the `auditEvent` signature already used elsewhere in the file (match arg order). Confirm `AuditAction` has a suitable code; if a dedicated mirror action exists, use it, else `CMD_ACEITO`.

- [ ] **Step 3: Add endpoints in IrrigationWebEndpoints.cpp**

Mirror the existing zone-endpoint pattern (parse body → module apply → respond). Add handlers and register them (both registration sites, matching how `hZones*` are registered):

```cpp
// GET /api/irrigation/mirror
static void hMirror(...) {
    char buf[1024];
    size_t n = irrigationModule->gwBuildMirror(buf, sizeof(buf));
    // respond application/json with buf[0..n]  (mirror hAudit/hExport response idiom)
}
// POST /api/irrigation/mirror  {enabled}
static void hMirrorToggle(...) {
    bool en; ParseResult pr = parseMirrorToggle(body, nb, en);
    if (!pr.ok) { /* 400 pr.err */ return; }
    irrigationModule->gwSetMirrorEnabled(en);
    // respond gwBuildMirror(...)
}
// POST /api/irrigation/mirror/mapping  {input,zoneId,invertido,habilitado}
static void hMirrorMapping(...) {
    int8_t in; uint8_t zid; bool inv, hab;
    ParseResult pr = parseMirrorMapping(body, nb, in, zid, inv, hab);
    if (!pr.ok) { /* 400 */ return; }
    char err[48];
    if (!irrigationModule->gwApplyMirrorMapping(in, zid, inv, hab, err, sizeof(err))) { /* 400 err */ return; }
    // respond gwBuildMirror(...)
}
// POST /api/irrigation/mirror/mapping/delete  {input}
static void hMirrorMappingDelete(...) {
    int8_t in; uint8_t zid; bool inv, hab; // reuse parser; only input needed
    ParseResult pr = parseMirrorMapping(body, nb, in, zid, inv, hab);
    if (!pr.ok) { /* accept minimal {input} — parse input directly if needed */ }
    irrigationModule->gwDeleteMirrorMapping(in);
    // respond gwBuildMirror(...)
}
```

All gated `role==GATEWAY` (mirror the guard used by `hExport`/`hZones`). Copy the exact request/response plumbing (path registration, method check, body read, content-type) from the neighbouring zone/interlock handlers — do not invent new plumbing.

- [ ] **Step 4: Verify full native suite unaffected + module compiles**

Endpoints are excluded from native; module glue compiles in native. Run the FULL suite in Docker:
```bash
./bin/run-tests.sh
```
Expected: `RESULT: GREEN` (same suite count as baseline).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp src/modules/irrigation/IrrigationWebEndpoints.cpp
git commit -m "feat(irrigation): mirror endpoints + gateway glue (CI-only)"
```

---

### Task 5: Frontend — tela Espelhamento + mock

**Files:**
- Modify: `data/irrigacao/app.js`, `data/irrigacao/mock.js` (and `data/irrigacao/style.css` only if a needed token is missing)

**Interfaces:**
- Consumes: `GET/POST /api/irrigation/mirror`, `POST /api/irrigation/mirror/mapping[/delete]` (Task 4).

- [ ] **Step 1: Add `renderEspelhamento` reproducing the mockup**

In `data/irrigacao/app.js`, add `renderEspelhamento()` reproducing the mockup screen at
`C:\Users\Jefferson\Downloads\Irrigacao Mobile.dc.html` lines 815–970: list view (toggle
`Ativar espelhamento`, "Bypass ativo" banner when enabled, "Estado atual das portas" live
list of 4 ports read-only, "Associações porta → zona" with `+ Nova associação`, per-mapping
card with INV badge / Habilitada-Desativada / enable toggle) and edit view (porta pills of
free ports, zona pills, polaridade Normal/Invertido, habilitada toggle, Salvar, Excluir with
confirm). Follow the existing `renderNiveis`/`renderStations` idioms in this file for state
caching that survives the 3 s poll (module-level cache object + re-render from cache;
POST then refetch). Data comes from `GET /api/irrigation/mirror`. The live port state is
**read-only** (no "toque para simular" — that line is mock-only).

- [ ] **Step 2: Wire into "Mais" navigation**

Add `espelhamento: renderEspelhamento` to the `RENDER` map, `espelhamento: 'Espelhamento'`
to `SECTION_LABELS`, and an entry in the "Mais" menu list (`renderMais`) that calls
`showSub('espelhamento')` — matching how `grupos`/`niveis` are wired. Add `espelhamento` to
the poll-refresh set (POLLED) so the live port state updates every 3 s.

- [ ] **Step 3: Mock support**

In `data/irrigacao/mock.js`, add handlers for `GET /api/irrigation/mirror` (return
`{enabled, ports:[…4…], }` consistent with `STATE.zones`), `POST /api/irrigation/mirror`
(flip enabled), `POST /api/irrigation/mirror/mapping` (set zone.fonteInput/enabled +
polarity), `POST /api/irrigation/mirror/mapping/delete`. Keep the two-rule consistency
style of the existing mock.

- [ ] **Step 4: Verify (no src/ changes → native unaffected)**

Confirm `git diff --name-only` shows only `data/irrigacao/*`. Visual check in browser with
mock (uncomment the mock include in `index.html` locally; do NOT commit that toggle) is a
manual follow-up.

- [ ] **Step 5: Commit**

```bash
git add data/irrigacao/app.js data/irrigacao/mock.js
git commit -m "feat(irrigation): painel — tela Modo Espelhamento (Mais)"
```

---

## Notes for the executor

- Endpoints/frontend are CI/bancada-only; native suite validates only the pure helpers (Tasks 1–3) + module compile (Task 4).
- **Banca 2+ nós exigida**: entrada ativa → válvula do nó abre, bypass do scheduler, pausa por associação — não são native-testáveis.
- Match the actual `JsonReader`/`JsonWriter`/`ParseResult`/`auditEvent`/persistence APIs in the files you touch — the code blocks above use the intended shapes; verify exact method names against neighbours before compiling.
- Do NOT run `trunk fmt` on Windows host.
