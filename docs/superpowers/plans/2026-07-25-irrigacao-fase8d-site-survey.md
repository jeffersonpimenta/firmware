# Irrigação Fase 8d — Site survey (§8.5) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A node in beacon mode emits `PING_SURVEY` (new `kind=2`) at an interval; the gateway logs each received beacon (SNR/RSSI + optional coordinate) into a RAM ring; the gateway panel shows a "Cobertura" table — planning station/repeater positions before install (§8.5).

**Architecture:** Three pure, native-tested units — `SurveyBeacon` (interval + auto-expire state machine), `SurveyLog` (RAM ring of `SurveyPoint`), and `IrrigationWeb::buildSurvey`/`parseSurveyStart`. `IrrigationModule` is the only hardware seam: it ticks the beacon and emits, logs `kind=2` on the gateway, and exposes accessors. HTTP endpoints (gateway panel + node portal §7.2 + SERVICO portal §11.8) and static frontend are CI/bench-only (excluded from the native build).

**Tech Stack:** C++17, PlatformIO, Unity (native suites under `test/`), Meshtastic mesh packet API, hand-rolled JSON (`IrrigationWeb::JsonWriter`/`JsonReader`), vanilla JS frontend.

**Design doc:** `docs/superpowers/specs/2026-07-25-irrigacao-fase8d-site-survey-design.md`

## Global Constraints

Copy into every task's mental checklist:

- **No new wire type. Protocol VERSION stays 1.** `kind=2` (BEACON) is a new *value* of the existing `PingSurvey.kind`; `encodePingSurvey`/`decodePingSurvey` already serialize every field — **no codec change**.
- **Settings ABI v5 (176 B) unchanged.** Beacon state is RAM-only; no persisted field added.
- **No new dependency.** JSON hand-rolled (`IrrigationWeb::JsonWriter`/`JsonReader`).
- **Pure-core / thin-glue.** Radio/`millis()`/web-server code lives in `IrrigationModule` or the endpoint files. Endpoint files (`IrrigationWebEndpoints.cpp`, `IrrigationPortalEndpoints.cpp`, `ServicePortalEndpoints.cpp`) are already excluded from the native build in `variants/native/portduino.ini` — survey endpoints reuse them, **no new exclusion needed**.
- **Nodes never reply to `kind=2`.** The gateway logs it passively; logging is read-only, exempt from auth/seq (like `handleResyncSeq`).
- **Fail-safe ceiling 120 min** and all station rules remain; survey bypasses nothing.
- **Native verdict:** `./bin/run-tests.sh` → GREEN exit 0 (full); `-f <suite>` → FILTERED exit 3 (single). On Windows run via Docker (memory `windows-native-test-docker`: `MSYS_NO_PATHCONV=1` + Docker Desktop). Bump `test/native-suite-count` per new suite.
- **Format** with `trunk fmt` before each commit (the host is Windows and cannot run trunk fmt — write code already matching the repo's clang-format; CI validates). Commit messages: `feat(irrigation): 8d …`.
- **Bench 2+ nodes REQUIRED before field use** — beacon emission, gateway logging, SNR/RSSI capture over the radio are not native-testable.

## File Structure

**New (always compiled — pure, native-tested):**
- `src/modules/irrigation/SurveyBeacon.h` — header-only beacon state machine.
- `src/modules/irrigation/SurveyLog.h` — header-only `SurveyPoint` + RAM ring.

**New test suites:** `test/test_survey_beacon/test_main.cpp`, `test/test_survey_log/test_main.cpp`.

**Modified:**
- `src/modules/irrigation/IrrigationWebApi.h` / `.cpp` — `buildSurvey` + `parseSurveyStart` + `SurveyStartReq`.
- `test/test_irrigation_webapi/test_main.cpp` — cases for the two above.
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — own `SurveyBeacon`+`SurveyLog`; tick-emit; gateway log on `kind=2`; accessors `portalStartSurvey`/`portalStopSurvey`/`buildSurveyLog`/`clearSurveyLog`; `emitSurveyBeacon`.
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — `GET /api/irrigation/survey` + `POST /api/irrigation/survey/clear`.
- `src/modules/irrigation/IrrigationPortalEndpoints.cpp` — `POST /api/portal/survey/{start,stop}`.
- `src/modules/irrigation/ServicePortalEndpoints.cpp` — `POST /api/portal/service/survey/{start,stop}`.
- `data/irrigacao/index.html` + `app.js` — "Cobertura" tab (survey log table + clear).
- `data/irrigacao/portal/index.html` + `app.js` — "Cobertura" tab (beacon start/stop control, role-aware endpoint).
- `test/native-suite-count` — 60 → 62.

---

### Task 1: `SurveyBeacon` pure state machine + suite

**Files:**
- Create: `src/modules/irrigation/SurveyBeacon.h`
- Test: `test/test_survey_beacon/test_main.cpp`
- Modify: `test/native-suite-count`

**Interfaces:**
- Produces: `class SurveyBeacon` with `void start(uint32_t nowMs, uint16_t intervalS, uint16_t timeoutS, int32_t latE7, int32_t lonE7, bool hasCoord)`, `void stop()`, `bool active(uint32_t nowMs) const`, `bool tick(uint32_t nowMs)`, `int32_t latE7() const`, `int32_t lonE7() const`, `bool hasCoord() const`.

- [ ] **Step 1: Write the failing test**

`test/test_survey_beacon/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/SurveyBeacon.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_beacon_fires_first_immediately_then_at_interval()
{
    SurveyBeacon b;
    b.start(1000, 5, 60, 0, 0, false);
    TEST_ASSERT_TRUE(b.tick(1000));  // imediato
    TEST_ASSERT_FALSE(b.tick(1001)); // antes do próximo
    TEST_ASSERT_FALSE(b.tick(5999));
    TEST_ASSERT_TRUE(b.tick(6000));  // +5 s
    TEST_ASSERT_TRUE(b.tick(11000)); // +5 s
}

static void test_beacon_auto_expires_at_timeout()
{
    SurveyBeacon b;
    b.start(0, 5, 10, 0, 0, false); // timeout 10 s
    TEST_ASSERT_TRUE(b.tick(0));
    TEST_ASSERT_TRUE(b.active(9000));
    TEST_ASSERT_FALSE(b.tick(11000)); // expirou
    TEST_ASSERT_FALSE(b.active(11000));
}

static void test_beacon_stop_silences()
{
    SurveyBeacon b;
    b.start(0, 5, 60, 0, 0, false);
    b.stop();
    TEST_ASSERT_FALSE(b.tick(0));
    TEST_ASSERT_FALSE(b.active(0));
}

static void test_beacon_coord_getters()
{
    SurveyBeacon b;
    b.start(0, 5, 60, -221000000, -476000000, true);
    TEST_ASSERT_TRUE(b.hasCoord());
    TEST_ASSERT_EQUAL_INT32(-221000000, b.latE7());
    TEST_ASSERT_EQUAL_INT32(-476000000, b.lonE7());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_beacon_fires_first_immediately_then_at_interval);
    RUN_TEST(test_beacon_auto_expires_at_timeout);
    RUN_TEST(test_beacon_stop_silences);
    RUN_TEST(test_beacon_coord_getters);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_survey_beacon`
Expected: RED — `SurveyBeacon.h` not found / undefined.

- [ ] **Step 3: Write minimal implementation**

`src/modules/irrigation/SurveyBeacon.h`:
```cpp
#pragma once
#include <cstddef>
#include <cstdint>

// §8.5 site survey: beacon-mode state machine. Pure, no hardware.
// A node in beacon mode emits PING_SURVEY (kind=2) every intervalS seconds
// until timeoutS elapses. tick() tells the caller when to emit.
class SurveyBeacon
{
  public:
    void start(uint32_t nowMs, uint16_t intervalS, uint16_t timeoutS, int32_t latE7v, int32_t lonE7v, bool hasCoordV)
    {
        on = true;
        intervalMs = (uint32_t)intervalS * 1000;
        expireAtMs = nowMs + (uint32_t)timeoutS * 1000;
        nextEmitMs = nowMs; // primeiro beacon imediato
        lat = latE7v;
        lon = lonE7v;
        coord = hasCoordV;
    }
    void stop() { on = false; }
    bool active(uint32_t nowMs) const { return on && nowMs < expireAtMs; }
    // true no máximo 1×/intervalo enquanto ativo; avança o cronograma; auto-para no timeout.
    bool tick(uint32_t nowMs)
    {
        if (!on)
            return false;
        if (nowMs >= expireAtMs) {
            on = false;
            return false;
        }
        if (nowMs < nextEmitMs)
            return false;
        nextEmitMs += intervalMs ? intervalMs : 1000;
        return true;
    }
    int32_t latE7() const { return lat; }
    int32_t lonE7() const { return lon; }
    bool hasCoord() const { return coord; }

  private:
    bool on = false;
    uint32_t nextEmitMs = 0, expireAtMs = 0, intervalMs = 0;
    int32_t lat = 0, lon = 0;
    bool coord = false;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_survey_beacon`
Expected: FILTERED (exit 3), all 4 cases pass.

- [ ] **Step 5: Bump suite count + commit**

Set `test/native-suite-count` to `61`.
```bash
trunk fmt src/modules/irrigation/SurveyBeacon.h test/test_survey_beacon/test_main.cpp
git add src/modules/irrigation/SurveyBeacon.h test/test_survey_beacon/ test/native-suite-count
git commit -m "feat(irrigation): 8d SurveyBeacon interval/expire state machine"
```

---

### Task 2: `SurveyLog` RAM ring + suite

**Files:**
- Create: `src/modules/irrigation/SurveyLog.h`
- Test: `test/test_survey_log/test_main.cpp`
- Modify: `test/native-suite-count`

**Interfaces:**
- Produces:
```cpp
struct SurveyPoint {
    uint32_t node; uint8_t role; uint16_t vbatCentiV; uint16_t fwVersion;
    int32_t latE7, lonE7; bool hasCoord;
    int8_t snrQuarterDb; int16_t rssiDbm; uint32_t uptimeS;
};
class SurveyLog {
  public:
    static constexpr size_t CAP = 32;
    void add(const SurveyPoint &p);
    size_t count() const;
    const SurveyPoint &at(size_t i) const;   // 0 = oldest
    void clear();
};
```

- [ ] **Step 1: Write the failing test**

`test/test_survey_log/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/SurveyLog.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static SurveyPoint pt(uint32_t node)
{
    SurveyPoint p{};
    p.node = node;
    return p;
}

static void test_log_add_and_order()
{
    SurveyLog log;
    log.add(pt(1));
    log.add(pt(2));
    log.add(pt(3));
    TEST_ASSERT_EQUAL_size_t(3, log.count());
    TEST_ASSERT_EQUAL_UINT32(1, log.at(0).node); // mais antigo
    TEST_ASSERT_EQUAL_UINT32(3, log.at(2).node); // mais novo
}

static void test_log_overflow_evicts_oldest()
{
    SurveyLog log;
    for (uint32_t i = 1; i <= SurveyLog::CAP + 2; i++)
        log.add(pt(i));
    TEST_ASSERT_EQUAL_size_t(SurveyLog::CAP, log.count());
    TEST_ASSERT_EQUAL_UINT32(3, log.at(0).node); // 1 e 2 despejados
    TEST_ASSERT_EQUAL_UINT32(SurveyLog::CAP + 2, log.at(SurveyLog::CAP - 1).node);
}

static void test_log_clear()
{
    SurveyLog log;
    log.add(pt(1));
    log.clear();
    TEST_ASSERT_EQUAL_size_t(0, log.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_log_add_and_order);
    RUN_TEST(test_log_overflow_evicts_oldest);
    RUN_TEST(test_log_clear);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_survey_log`
Expected: RED — `SurveyLog.h` not found.

- [ ] **Step 3: Write minimal implementation**

`src/modules/irrigation/SurveyLog.h`:
```cpp
#pragma once
#include <cstddef>
#include <cstdint>

// §8.5 site survey: uma recepção de beacon registrada no gateway.
struct SurveyPoint {
    uint32_t node = 0;
    uint8_t role = 0;
    uint16_t vbatCentiV = 0;
    uint16_t fwVersion = 0;
    int32_t latE7 = 0, lonE7 = 0;
    bool hasCoord = false;
    int8_t snrQuarterDb = 0;
    int16_t rssiDbm = 0;
    uint32_t uptimeS = 0; // uptime do gateway (s) na captura; sem RTC assumido
};

// Ring em RAM; mantém o mais novo, despeja o mais antigo em CAP. Append (sem dedup):
// o mesmo nó portátil medido em pontos diferentes gera linhas distintas.
class SurveyLog
{
  public:
    static constexpr size_t CAP = 32;
    void add(const SurveyPoint &p)
    {
        ring[head] = p;
        head = (head + 1) % CAP;
        if (n < CAP)
            n++;
    }
    size_t count() const { return n; }
    const SurveyPoint &at(size_t i) const
    {
        size_t start = (n == CAP) ? head : 0; // cheio → head aponta pro mais antigo
        return ring[(start + i) % CAP];
    }
    void clear()
    {
        head = 0;
        n = 0;
    }

  private:
    SurveyPoint ring[CAP];
    size_t head = 0, n = 0;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_survey_log`
Expected: FILTERED, all 3 cases pass.

- [ ] **Step 5: Bump suite count + commit**

Set `test/native-suite-count` to `62`.
```bash
trunk fmt src/modules/irrigation/SurveyLog.h test/test_survey_log/test_main.cpp
git add src/modules/irrigation/SurveyLog.h test/test_survey_log/ test/native-suite-count
git commit -m "feat(irrigation): 8d SurveyLog RAM ring of SurveyPoint"
```

---

### Task 3: `buildSurvey` + `parseSurveyStart` (web API, pure)

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`, `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `SurveyPoint` (from `SurveyLog.h`), `JsonWriter`, `JsonReader`, `ParseResult`.
- Produces (namespace `IrrigationWeb`):
```cpp
size_t buildSurvey(const SurveyPoint *pts, size_t n, uint32_t nowS, char *buf, size_t cap);
struct SurveyStartReq { uint16_t intervalS = 5; uint16_t timeoutS = 300; int32_t latE7 = 0, lonE7 = 0; bool hasCoord = false; };
ParseResult parseSurveyStart(const char *json, size_t len, SurveyStartReq &out);
```

- [ ] **Step 1: Write the failing test** (append to `test/test_irrigation_webapi/test_main.cpp`, register in `setup()`)

```cpp
// ── Fase 8d — site survey ────────────────────────────────────────────────────
static void test_buildSurvey_two_points()
{
    SurveyPoint pts[2] = {};
    pts[0].node = 0xa1b2c3d4;
    pts[0].role = 0;
    pts[0].snrQuarterDb = 20; // 5 dB
    pts[0].rssiDbm = -95;
    pts[0].hasCoord = true;
    pts[0].latE7 = -221000000;
    pts[0].lonE7 = -476000000;
    pts[0].uptimeS = 40;
    pts[1].node = 0xe5f6a7b8;
    pts[1].uptimeS = 90;
    char buf[512];
    size_t n = IrrigationWeb::buildSurvey(pts, 2, /*nowS*/ 100, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = 0;
    TEST_ASSERT_TRUE(buf[0] == '[');
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"no\":"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"snr\":20"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"idadeS\":60")); // 100 - 40
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"coord\":true"));
}

static void test_parseSurveyStart_full()
{
    const char *body = "{\"intervalS\":10,\"timeoutS\":600,\"lat\":-221000000,\"lon\":-476000000}";
    IrrigationWeb::SurveyStartReq r;
    IrrigationWeb::ParseResult pr = IrrigationWeb::parseSurveyStart(body, strlen(body), r);
    TEST_ASSERT_TRUE(pr.ok);
    TEST_ASSERT_EQUAL_UINT16(10, r.intervalS);
    TEST_ASSERT_EQUAL_UINT16(600, r.timeoutS);
    TEST_ASSERT_TRUE(r.hasCoord);
    TEST_ASSERT_EQUAL_INT32(-221000000, r.latE7);
    TEST_ASSERT_EQUAL_INT32(-476000000, r.lonE7);
}

static void test_parseSurveyStart_defaults_and_nocoord()
{
    const char *body = "{}";
    IrrigationWeb::SurveyStartReq r;
    IrrigationWeb::parseSurveyStart(body, strlen(body), r);
    TEST_ASSERT_EQUAL_UINT16(5, r.intervalS);
    TEST_ASSERT_EQUAL_UINT16(300, r.timeoutS);
    TEST_ASSERT_FALSE(r.hasCoord);
}

static void test_parseSurveyStart_clamps()
{
    const char *body = "{\"intervalS\":0,\"timeoutS\":99999}";
    IrrigationWeb::SurveyStartReq r;
    IrrigationWeb::parseSurveyStart(body, strlen(body), r);
    TEST_ASSERT_EQUAL_UINT16(1, r.intervalS);
    TEST_ASSERT_EQUAL_UINT16(3600, r.timeoutS);
}
```
Register in `setup()`:
```cpp
    RUN_TEST(test_buildSurvey_two_points);
    RUN_TEST(test_parseSurveyStart_full);
    RUN_TEST(test_parseSurveyStart_defaults_and_nocoord);
    RUN_TEST(test_parseSurveyStart_clamps);
```
(If `<cstring>` for `strstr`/`strlen` isn't already included at the top of the test file, add `#include <cstring>`.)

- [ ] **Step 2: Run test to verify it fails**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: RED — `buildSurvey`/`parseSurveyStart` undefined.

- [ ] **Step 3: Write minimal implementation**

In `IrrigationWebApi.h`, add the include near the other module includes at the top:
```cpp
#include "modules/irrigation/SurveyLog.h"
```
and inside `namespace IrrigationWeb`, after the groups section (before the closing `}`):
```cpp
// ── Fase 8d — site survey (§8.5) ─────────────────────────────────────────────
// Serializa pontos de survey p/ a tabela Cobertura do painel. idadeS = nowS - uptimeS.
size_t buildSurvey(const SurveyPoint *pts, size_t n, uint32_t nowS, char *buf, size_t cap);

struct SurveyStartReq {
    uint16_t intervalS = 5;
    uint16_t timeoutS = 300;
    int32_t latE7 = 0, lonE7 = 0;
    bool hasCoord = false;
};
// {intervalS?, timeoutS?, lat?, lon?}. Defaults 5 s / 300 s; clamp [1,3600].
// hasCoord = lat E lon presentes. Nunca falha (pr.ok sempre true).
ParseResult parseSurveyStart(const char *json, size_t len, SurveyStartReq &out);
```

In `IrrigationWebApi.cpp`, add (end of `namespace IrrigationWeb`, before the closing brace):
```cpp
size_t buildSurvey(const SurveyPoint *pts, size_t n, uint32_t nowS, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const SurveyPoint &p = pts[i];
        w.beginObject();
        w.keyNum("no", p.node);
        w.keyNum("role", p.role);
        w.keyNum("vbat", p.vbatCentiV);
        w.keyNum("fw", p.fwVersion);
        w.keyNum("lat", p.latE7);
        w.keyNum("lon", p.lonE7);
        w.keyBool("coord", p.hasCoord);
        w.keyNum("snr", p.snrQuarterDb);
        w.keyNum("rssi", p.rssiDbm);
        w.keyNum("idadeS", (int64_t)(nowS >= p.uptimeS ? nowS - p.uptimeS : 0));
        w.endObject();
    }
    w.endArray();
    return w.done();
}

ParseResult parseSurveyStart(const char *json, size_t len, SurveyStartReq &out)
{
    ParseResult pr;
    JsonReader rd(json, len);
    int64_t v = 0;
    if (rd.getInt("intervalS", v))
        out.intervalS = (uint16_t)(v < 1 ? 1 : (v > 3600 ? 3600 : v));
    if (rd.getInt("timeoutS", v))
        out.timeoutS = (uint16_t)(v < 1 ? 1 : (v > 3600 ? 3600 : v));
    int64_t lat = 0, lon = 0;
    bool hasLat = rd.getInt("lat", lat), hasLon = rd.getInt("lon", lon);
    if (hasLat && hasLon) {
        out.latE7 = (int32_t)lat;
        out.lonE7 = (int32_t)lon;
        out.hasCoord = true;
    }
    return pr;
}
```
(Defaults live in the `SurveyStartReq` member initializers, so absent keys keep 5/300 and `hasCoord=false`.)

- [ ] **Step 4: Run test to verify it passes**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FILTERED, all cases (existing + 4 new) pass.

- [ ] **Step 5: Commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.* test/test_irrigation_webapi/
git commit -m "feat(irrigation): 8d buildSurvey + parseSurveyStart (web API)"
```

---

### Task 4: Module glue — beacon emit + gateway log + accessors

Not native-unit-testable (radio/module globals, as in 8b/8c glue). The gate is: **the module compiles into the native build and the full suite stays GREEN.**

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `src/modules/irrigation/IrrigationModule.cpp`

**Interfaces:**
- Consumes: `SurveyBeacon`, `SurveyLog`, `SurveyPoint`, `IrrigationWeb::{buildSurvey, SurveyStartReq}`, existing `PingSurvey`/`encodePingSurvey`/`setServiceFlag`/`allocDataPacket`/`service->sendToMesh`/`batteryCentiV()`/`gwIsGateway()`.
- Produces (public): `bool portalStartSurvey(const IrrigationWeb::SurveyStartReq &r)`, `void portalStopSurvey()`, `size_t buildSurveyLog(char *buf, size_t cap)`, `void clearSurveyLog()`.

- [ ] **Step 1: Header edits** — `src/modules/irrigation/IrrigationModule.h`

Add includes near the other irrigation includes (after `#include "modules/irrigation/SensorSampler.h"`):
```cpp
#include "modules/irrigation/SurveyBeacon.h"
#include "modules/irrigation/SurveyLog.h"
```
Add `SurveyStartReq` to the `IrrigationWeb` forward-decl block (near `struct WebCommand;`):
```cpp
struct SurveyStartReq;
```
Add public accessors (after the Fase 8c `svcPortal…` block, before `protected:`):
```cpp
    // Fase 8d — site survey (§8.5). Portais (nó §7.2 / SERVICO §11.8) iniciam o beacon;
    // o painel do gateway lê o log. portalStart/Stop valem p/ qualquer papel que beacona.
    bool portalStartSurvey(const IrrigationWeb::SurveyStartReq &r);
    void portalStopSurvey();
    size_t buildSurveyLog(char *buf, size_t cap); // GATEWAY: tabela Cobertura do painel
    void clearSurveyLog();
```
Add private members + helper (near the `svc…` members / other private helpers):
```cpp
    // Fase 8d — site survey (§8.5): beacon (qualquer papel que beacona) + log (gateway).
    SurveyBeacon surveyBeacon;
    SurveyLog surveyLog;
    void emitSurveyBeacon(); // PING_SURVEY kind=2 broadcast (nodeinfo + coordenada)
```

- [ ] **Step 2: Tick — emit on schedule** — in `IrrigationModule.cpp`, inside `runOnce()`, near the other periodic work (e.g. next to `refreshLedMode();` / `gwTick()`):
```cpp
    if (surveyBeacon.tick(millis()))
        emitSurveyBeacon();
```

- [ ] **Step 3: Gateway logs `kind=2`** — in `handlePingSurvey`, insert a branch immediately after the `decodePingSurvey(...)` guard and **before** the existing `if (req.kind != 0)` REPLY branch:
```cpp
    if (req.kind == 2) { // BEACON (§8.5): gateway loga; ninguém responde
        if (gwIsGateway()) {
            SurveyPoint sp{};
            sp.node = mp.from;
            sp.role = req.role;
            sp.vbatCentiV = req.vbatCentiV;
            sp.fwVersion = req.fwVersion;
            sp.latE7 = req.latE7;
            sp.lonE7 = req.lonE7;
            sp.hasCoord = (req.latE7 != 0 || req.lonE7 != 0);
            sp.snrQuarterDb = (int8_t)(mp.rx_snr * 4);
            sp.rssiDbm = (int16_t)mp.rx_rssi;
            sp.uptimeS = millis() / 1000;
            surveyLog.add(sp);
        }
        return;
    }
```

- [ ] **Step 4: Implement emit + accessors** — add near `svcEmitProbe` in `IrrigationModule.cpp`:
```cpp
void IrrigationModule::emitSurveyBeacon()
{
    PingSurvey b = {};
    b.kind = 2; // BEACON (§8.5)
    b.role = settings.role;
    b.configEpoch = settings.configEpoch;
    b.vbatCentiV = batteryCentiV();
    b.fwVersion = APP_FW_VERSION;
    b.latE7 = surveyBeacon.hasCoord() ? surveyBeacon.latE7() : 0;
    b.lonE7 = surveyBeacon.hasCoord() ? surveyBeacon.lonE7() : 0;

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    uint16_t sz =
        (uint16_t)encodePingSurvey(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, b);
    if (!sz) {
        packetPool.release(p);
        return;
    }
    p->decoded.payload.size = sz;
    if ((IrrigationRole)settings.role == IrrigationRole::SERVICO)
        setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
    service->sendToMesh(p, RX_SRC_LOCAL, false);
}

bool IrrigationModule::portalStartSurvey(const IrrigationWeb::SurveyStartReq &r)
{
    surveyBeacon.start(millis(), r.intervalS, r.timeoutS, r.latE7, r.lonE7, r.hasCoord);
    return true;
}

void IrrigationModule::portalStopSurvey()
{
    surveyBeacon.stop();
}

size_t IrrigationModule::buildSurveyLog(char *buf, size_t cap)
{
    SurveyPoint tmp[SurveyLog::CAP];
    size_t k = surveyLog.count();
    for (size_t i = 0; i < k; i++)
        tmp[i] = surveyLog.at(i);
    return IrrigationWeb::buildSurvey(tmp, k, millis() / 1000, buf, cap);
}

void IrrigationModule::clearSurveyLog()
{
    surveyLog.clear();
}
```
(`IrrigationModule.cpp` already includes `IrrigationWebApi.h` and uses `NODENUM_BROADCAST`/`allocDataPacket`/`packetPool`/`RX_SRC_LOCAL`/`IrrigationRole` — same symbols `svcEmitProbe` uses. `mp.rx_rssi` is the sibling of the already-used `mp.rx_snr` on `meshtastic_MeshPacket`.)

- [ ] **Step 5: Run the full native suite**

Run: `./bin/run-tests.sh`
Expected: GREEN exit 0 — the module compiles into the native build and all 62 suites pass (no behavior change to existing suites; the new pure suites from Tasks 1–2 and webapi cases from Task 3 pass).

- [ ] **Step 6: Commit**

```bash
trunk fmt src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git add src/modules/irrigation/IrrigationModule.*
git commit -m "feat(irrigation): 8d module glue — beacon emit + gateway survey log"
```

---

### Task 5: HTTP endpoints (gateway panel + node portal §7.2 + SERVICO portal §11.8)

CI/bench-only — all three files are excluded from the native build (`variants/native/portduino.ini`). No native test exercises them; the gate is that the full suite still builds+passes (proves nothing native references them by accident).

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp`, `src/modules/irrigation/IrrigationPortalEndpoints.cpp`, `src/modules/irrigation/ServicePortalEndpoints.cpp`

**Interfaces:**
- Consumes: `irrigationModule->{buildSurveyLog, clearSurveyLog, portalStartSurvey, portalStopSurvey}`, `IrrigationWeb::{parseSurveyStart, SurveyStartReq, ParseResult}`.

- [ ] **Step 1: Gateway panel endpoints** — `IrrigationWebEndpoints.cpp`, add handlers before the `registerIrrigationHandlers` function:
```cpp
// GET /api/irrigation/survey — pontos de cobertura registrados (§8.5). GATEWAY-only.
static void hSurvey(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    const size_t cap = 8192;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = irrigationModule->buildSurveyLog(buf, cap); // CI-only
    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
    free(buf);
}

// POST /api/irrigation/survey/clear — zera o log de cobertura.
static void hSurveyClear(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    irrigationModule->clearSurveyLog();
    sendJson(res, "{\"ok\":true}");
}
```
Register (inside `registerIrrigationHandlers`, after the export line):
```cpp
    // Fase 8d: site survey (§8.5)
    server->registerNode(new ResourceNode("/api/irrigation/survey", "GET", &hSurvey));
    server->registerNode(new ResourceNode("/api/irrigation/survey/clear", "POST", &hSurveyClear));
```

- [ ] **Step 2: Node portal endpoints §7.2** — `IrrigationPortalEndpoints.cpp`, add handlers before `registerIrrigationPortalHandlers` (this file has `using namespace IrrigationWeb;` and the `readBody`/`sendJson`/`sendParseErrors` helpers, mirroring the other endpoint files):
```cpp
// POST /api/portal/survey/start — entra em modo beacon de cobertura (§8.5).
static void hSurveyStart(HTTPRequest *req, HTTPResponse *res)
{
    char body[192];
    size_t nb = readBody(req, body, sizeof body);
    SurveyStartReq r;
    ParseResult pr = parseSurveyStart(body, nb, r);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    irrigationModule->portalStartSurvey(r);
    sendJson(res, "{\"ok\":true}");
}

// POST /api/portal/survey/stop — sai do modo beacon.
static void hSurveyStop(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    irrigationModule->portalStopSurvey();
    sendJson(res, "{\"ok\":true}");
}
```
Register (inside `registerIrrigationPortalHandlers`):
```cpp
    server->registerNode(new ResourceNode("/api/portal/survey/start", "POST", &hSurveyStart));
    server->registerNode(new ResourceNode("/api/portal/survey/stop", "POST", &hSurveyStop));
```
(If this file lacks a `parseSurveyStart` visible declaration, it already `#include`s `IrrigationWebApi.h` via the module header; the symbol is in `IrrigationWeb`, covered by the file's `using namespace IrrigationWeb;`.)

- [ ] **Step 3: SERVICO portal endpoints §11.8** — `ServicePortalEndpoints.cpp`, add handlers before `registerIrrigationServicePortalHandlers` (uses the file's `svcGuard`):
```cpp
// POST /api/portal/service/survey/start — device SERVICO em modo beacon (§8.5).
static void hSvcSurveyStart(HTTPRequest *req, HTTPResponse *res)
{
    if (!svcGuard(res))
        return;
    char body[192];
    size_t nb = readBody(req, body, sizeof body);
    SurveyStartReq r;
    ParseResult pr = parseSurveyStart(body, nb, r);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    irrigationModule->portalStartSurvey(r);
    sendJson(res, "{\"ok\":true}");
}

static void hSvcSurveyStop(HTTPRequest *req, HTTPResponse *res)
{
    if (!svcGuard(res))
        return;
    irrigationModule->portalStopSurvey();
    sendJson(res, "{\"ok\":true}");
}
```
Register (inside `registerIrrigationServicePortalHandlers`):
```cpp
    server->registerNode(new ResourceNode("/api/portal/service/survey/start", "POST", &hSvcSurveyStart));
    server->registerNode(new ResourceNode("/api/portal/service/survey/stop", "POST", &hSvcSurveyStop));
```

- [ ] **Step 4: Verify native suite unaffected**

Run: `./bin/run-tests.sh`
Expected: GREEN exit 0 — endpoint files are excluded from the native build, so the suite is unchanged; this confirms the edits introduced no native reference.

- [ ] **Step 5: Commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebEndpoints.cpp src/modules/irrigation/IrrigationPortalEndpoints.cpp src/modules/irrigation/ServicePortalEndpoints.cpp
git add src/modules/irrigation/IrrigationWebEndpoints.cpp src/modules/irrigation/IrrigationPortalEndpoints.cpp src/modules/irrigation/ServicePortalEndpoints.cpp
git commit -m "feat(irrigation): 8d survey endpoints (panel + node portal + SERVICO portal)"
```

---

### Task 6: Frontend — gateway "Cobertura" tab + portal beacon control

Static assets; no automated test (bench validates in a browser).

**Files:**
- Modify: `data/irrigacao/index.html`, `data/irrigacao/app.js`, `data/irrigacao/portal/index.html`, `data/irrigacao/portal/app.js`

- [ ] **Step 1: Gateway panel tab button** — `data/irrigacao/index.html`, add after the `tamper` button:
```html
      <button data-tab="cobertura" class="tab">Cobertura</button>
```

- [ ] **Step 2: Gateway panel render + routing** — `data/irrigacao/app.js`:

Add the render function (near `renderAuditLog`):
```javascript
async function renderCobertura() {
  const COV_ROLES = ['Estação', 'Gateway', 'Repetidor', 'Serviço'];
  const rows = (await getJson('/survey')) || [];
  const list = Array.isArray(rows) ? rows : [];
  const clearRow = `<div class="log-export-row"><button class="btn ghost sm" id="cov-clear">Limpar</button></div>`;
  const body = list.length
    ? list.map((r) => {
        r = r || {};
        const coord = r.coord ? `${(num(r.lat) / 1e7).toFixed(5)}, ${(num(r.lon) / 1e7).toFixed(5)}` : '—';
        return `<tr>
          <td class="mono">${esc(nodeHex(r.no))}</td>
          <td>${esc(COV_ROLES[num(r.role)] || ('papel ' + num(r.role)))}</td>
          <td class="mono">${esc(coord)}</td>
          <td>${esc((num(r.snr) / 4).toFixed(0))}</td>
          <td>${esc(String(num(r.rssi)))}</td>
          <td>${esc(fmtSince(r.idadeS))}</td>
        </tr>`;
      }).join('')
    : '<tr><td colspan="6" class="empty">Sem beacons recebidos.</td></tr>';
  view.innerHTML = clearRow + `<div class="log-table-wrap"><table class="log-table">
    <thead><tr><th>Nó</th><th>Papel</th><th>Coordenada</th><th>SNR</th><th>RSSI</th><th>Idade</th></tr></thead>
    <tbody>${body}</tbody></table></div>`;
  const cb = view.querySelector('#cov-clear');
  if (cb)
    cb.addEventListener('click', async () => {
      await postJson('/survey/clear', {});
      renderCobertura().catch(() => {});
    });
}
```
Add to the `RENDER` map (after `tamper: renderTamper,`):
```javascript
  cobertura: renderCobertura,
```
In `show(tab)`, add `cobertura` to the 3-second poll set:
```javascript
  if (tab === 'overview' || tab === 'stations' || tab === 'sensores' || tab === 'cobertura') {
    timer = setInterval(() => fn().catch(() => {}), 3000);
  } else if (tab === 'grupos') {
```

- [ ] **Step 3: Portal tab button + section** — `data/irrigacao/portal/index.html`:

Add the tab button in `nav.tabs`, after the `net` button and before the `svc-only` buttons:
```html
    <button data-tab="survey" class="survey-tab">Cobertura</button>
```
Add the section after `#tab-net` (before the SERVICO sections):
```html
  <section id="tab-survey" class="tab hidden">
    <form id="surveyForm" class="card">
      <h2>Modo cobertura (site survey)</h2>
      <p class="muted">Emite beacons periódicos; o gateway registra SNR/RSSI de cada ponto.</p>
      <label>Intervalo (s) <input type="number" id="svInterval" min="1" max="3600" value="5" /></label>
      <label>Timeout (min) <input type="number" id="svTimeout" min="1" max="60" value="5" /></label>
      <label>Latitude <input type="number" id="svLat" step="0.0000001" value="0" /></label>
      <label>Longitude <input type="number" id="svLon" step="0.0000001" value="0" /></label>
      <button type="submit">Iniciar</button>
      <button type="button" id="svStop">Parar</button>
      <span id="svMsg" class="muted"></span>
    </form>
  </section>
```

- [ ] **Step 4: Portal wiring** — `data/irrigacao/portal/app.js`:

Declare a role tracker near the top (after `let svcInit = false;`):
```javascript
let lastRole = 0; // Fase 8d: última role vista, escolhe endpoint de survey
```
Record it in `refresh()` (inside the `if (ok) { … }` block, after `renderNode(body);`):
```javascript
    lastRole = body.role;
```
Keep the survey tab visible for the SERVICO device — in `initService()`, change the hide selector to spare `.survey-tab`:
```javascript
  document.querySelectorAll('nav.tabs button:not(.svc-only):not(.survey-tab)').forEach((b) => b.classList.add("hidden"));
```
Add the survey control at the end of the file:
```javascript
// ── Fase 8d — modo cobertura (site survey §8.5) ─────────────────────────────
const svBase = () => (lastRole === 3 ? "/api/portal/service/survey" : "/api/portal/survey");
document.getElementById("surveyForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const intervalS = +document.getElementById("svInterval").value;
  const timeoutS = Math.round(+document.getElementById("svTimeout").value * 60);
  const lat = +document.getElementById("svLat").value, lon = +document.getElementById("svLon").value;
  const payload = { intervalS, timeoutS };
  if (lat || lon) { payload.lat = Math.round(lat * 1e7); payload.lon = Math.round(lon * 1e7); }
  const { ok, body } = await j(svBase() + "/start", { method: "POST", body: JSON.stringify(payload) });
  document.getElementById("svMsg").textContent = ok ? "Beacon iniciado" : (body.errors || ["erro"]).join("; ");
});
document.getElementById("svStop").addEventListener("click", async () => {
  const { ok } = await j(svBase() + "/stop", { method: "POST" });
  document.getElementById("svMsg").textContent = ok ? "Beacon parado" : "erro";
});
```

- [ ] **Step 5: Sanity — native suite still green**

Run: `./bin/run-tests.sh`
Expected: GREEN exit 0 (frontend isn't compiled; this confirms no accidental breakage elsewhere).

- [ ] **Step 6: Commit**

```bash
trunk fmt data/irrigacao/index.html data/irrigacao/app.js data/irrigacao/portal/index.html data/irrigacao/portal/app.js
git add data/irrigacao/index.html data/irrigacao/app.js data/irrigacao/portal/index.html data/irrigacao/portal/app.js
git commit -m "feat(irrigation): 8d frontend — Cobertura tab (panel) + beacon control (portal)"
```

---

### Task 7: Full-suite gate + closeout

**Files:**
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (mark 8d done — match the existing roadmap style)

- [ ] **Step 1: Full native suite (Docker)**

Run: `./bin/run-tests.sh`
Expected: GREEN exit 0. Confirm the tail reports **62** suites (baseline 60 + `test_survey_beacon` + `test_survey_log`); `test_irrigation_webapi` carries the 4 new survey cases.

- [ ] **Step 2: Verify suite count file**

Confirm `test/native-suite-count` contains `62`.

- [ ] **Step 3: Roadmap note + commit**

Update the roadmap to mark Fase 8d (site survey §8.5) complete, noting: `kind=2` BEACON (additive, VERSION 1, ABI v5 intact), gateway RAM `SurveyLog`, panel "Cobertura" tab, beacon from node portal §7.2 + SERVICO portal §11.8, **bench 2+ nodes required before field** (radio path not native-testable).
```bash
git add docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "docs(irrigation): mark Fase 8d (site survey) done in roadmap"
```

- [ ] **Step 4: Push to fork** (user works from multiple computers — push after completion)

```bash
git push fork sistema-irrigacao
```

---

## Notes for the implementer

- **Placement freedom:** exact insertion points inside `runOnce()`, `handlePingSurvey`, and the `.cpp` accessor block can follow the surrounding code; the requirement is the branch order in `handlePingSurvey` (`kind==2` handled and `return`ed **before** the `kind != 0` REPLY branch) and that the tick calls `emitSurveyBeacon()`.
- **`buildSurveyLog` never returns 0 on an empty log** — `JsonWriter` writes `[]` (len 2), so `hSurvey` won't 500 when no beacons have arrived.
- **Bench test (post-merge, 2+ nodes):** put a station/SERVICO node in beacon mode via its portal (interval 5 s, a coordinate); confirm the gateway "Cobertura" tab fills with SNR/RSSI rows; confirm no reply storm (other nodes stay silent on `kind=2`); confirm auto-expiry after the timeout.
```
