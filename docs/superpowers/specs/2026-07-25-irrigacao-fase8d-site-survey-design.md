# Irrigação Fase 8d — Site survey (§8.5) — Design

**Status:** approved (brainstorm 2026-07-25), pre-implementation.

**Goal:** A candidate node (station/repeater/SERVICO tool) is put in *beacon mode* and
emits `PING_SURVEY` at a fixed interval at normal operating power. The gateway logs every
received beacon with SNR/RSSI and, if present, the test-point coordinate. The gateway panel
shows a "Cobertura" table of test points × link quality, so the installer can plan station
and repeater positions before the definitive install. Beacon mode auto-expires (configurable
timeout). Last sub-phase of Fase 8.

Spec reference: `myfork/especificacao-irrigacao-mesh.md` §8.5 (and §11.4 for the sibling scan
already built in 8b — distinct from this).

## Scope

**In:**
- New beacon semantics on the existing `PING_SURVEY` message (`kind=2 = BEACON`).
- A pure `SurveyBeacon` state machine (interval + auto-expire) that any beaconing role owns.
- A pure `SurveyLog` RAM ring on the gateway.
- Pure `IrrigationWeb::buildSurvey` + `parseSurveyStart`.
- Glue: node emits on tick; gateway logs on receive; CI-only endpoints; panel "Cobertura" tab;
  start/stop control on the node captive portal (§7.2) and the SERVICO portal (§11.8).

**Out (deferred / not this phase):**
- Gateway-panel-commands-a-remote-node beacon (would need a new gateway→node command / new wire
  semantics + device-side intake — breaks the "no new wire type" discipline of 8a/8b/8c).
- Flash-persisted survey log (RAM-only chosen; survey is a live install-time activity).
- GPS auto-fill of the coordinate (typed-in-portal, optional, is the MVP).
- Installer mode §8.4 (already cut earlier in Fase 8).
- Cipher/PSK-crypto, SD card — unrelated, deferred as in prior sub-phases.

## Decisions (brainstorm 2026-07-25)

1. **Beacon trigger surface:** node captive portal (§7.2) **and** SERVICO portal (§11.8). No
   panel-driven remote start (that needs a new command).
2. **Survey log storage:** RAM ring, transient. No flash. §8.5 does not require persistence;
   the installer reads the table live.
3. **Coordinate source:** typed in the portal, optional (`hasCoord`). The `PING_SURVEY` payload
   already carries `latE7`/`lonE7`.

## Global constraints (carried from 8a/8b/8c)

- **No new wire type. Protocol VERSION stays 1.** `kind=2` is a new *value* of the existing
  `PING_SURVEY.kind` field — additive; old nodes ignore an unknown kind.
- **Settings ABI v5 (176 B) unchanged.** Beacon state is RAM-only; no persisted field added.
- **No new dependency.** JSON hand-rolled (`IrrigationWeb::JsonWriter`/`JsonReader`).
- **Pure-core / thin-glue.** Anything touching radio / `millis()` source / web server lives
  behind the module seam or `#if !MESHTASTIC_EXCLUDE_WEBSERVER`; the native build compiles and
  passes without hardware. Endpoints are excluded from the native suite (as in 8c —
  `variants/native/portduino.ini` / `build_src_filter`).
- **Fail-safe ceiling 120 min** and all station-side rules remain; survey never bypasses them.
- **Native verdict:** `./bin/run-tests.sh` → GREEN exit 0 (Docker on Windows: `MSYS_NO_PATHCONV=1`
  + Docker Desktop). Bump `test/native-suite-count` per new suite. `trunk fmt` before commit
  (CI validates; host Windows cannot run it).
- **Bench 2+ nodes REQUIRED before field use** — beacon emission, gateway logging over the
  radio, SNR/RSSI capture, and re-tune interactions are not native-testable.

## Wire protocol

`PING_SURVEY` today (8a codec, `IrrigationProtocol`): `kind` + `role` + `configEpoch` +
`vbatCentiV` + `fwVersion` + `latE7` + `lonE7`. Existing kinds:

- `kind=0` **PROBE** — SERVICO scan sonda (§11.4). Recipients reply with `kind=1`. Rate-limited.
- `kind=1` **REPLY** — nodeinfo; collected only by the SERVICO prober (`onSurveyReply`).

Fase 8d adds:

- `kind=2` **BEACON** — self-emitted periodically by a node in survey-beacon mode, **broadcast**,
  populated with the sender's `role`/`configEpoch`/`vbatCentiV`/`fwVersion` and the test-point
  `latE7`/`lonE7` (zero when no coordinate). **No recipient replies to `kind=2`** — the gateway
  logs it passively. Logging is read-only, so it is exempt from the auth/seq gate (mirrors the
  `handleResyncSeq` exemption). A SERVICO-emitted beacon carries `FLAG_FROM_SERVICE`; the flag
  does not affect logging.

No codec/struct change is needed — `kind` is already a field; only a new accepted value and the
emit/log paths are added.

## New pure units (always compiled, native-tested)

### `src/modules/irrigation/SurveyBeacon.h` (header-only, like `RateLimiter`/`OpenGate`)

```cpp
class SurveyBeacon {
  public:
    void start(uint32_t nowMs, uint16_t intervalS, uint16_t timeoutS,
               int32_t latE7, int32_t lonE7, bool hasCoord);
    void stop();
    bool active(uint32_t nowMs) const;   // false once nowMs >= expireAt
    bool tick(uint32_t nowMs);           // true when an emit is due; advances next-emit; auto-stops at timeout
    int32_t latE7() const;  int32_t lonE7() const;  bool hasCoord() const;
  private:
    bool on = false;
    uint32_t nextEmitMs = 0, expireAtMs = 0, intervalMs = 0;
    int32_t lat = 0, lon = 0;
    bool coord = false;
};
```

Behaviour: `start` sets `on`, `intervalMs`, `expireAtMs = now + timeoutS*1000`, `nextEmitMs = now`
(first beacon immediate). `tick` returns true and bumps `nextEmitMs += intervalMs` when
`on && now >= nextEmitMs && now < expireAtMs`; when `now >= expireAtMs` it clears `on` and returns
false. `active` reflects the timeout. Millis wrap is out of scope (install-time, minutes).

Suite `test_survey_beacon`: fires at interval not before; multiple fires across time; auto-expire
clears active; `stop` silences; coordinate getters.

### `src/modules/irrigation/SurveyLog.h` (header-only RAM ring)

```cpp
struct SurveyPoint {
    uint32_t node;
    uint8_t role;
    uint16_t vbatCentiV;
    uint16_t fwVersion;
    int32_t latE7, lonE7;
    bool hasCoord;
    int8_t snrQuarterDb;   // rx_snr * 4
    int16_t rssiDbm;       // rx_rssi
    uint32_t uptimeS;      // gateway uptime at capture (no RTC assumed)
};

class SurveyLog {
  public:
    static constexpr size_t CAP = 32;
    void add(const SurveyPoint &p);   // append; oldest evicted at CAP
    size_t count() const;
    const SurveyPoint &at(size_t i) const;  // 0 = oldest
    void clear();
  private:
    SurveyPoint ring[CAP];
    size_t head = 0, n = 0;
};
```

Append (not dedup-by-node): the same portable node measured at different test points must produce
distinct rows. Newest-eviction ring, CAP 32.

Suite `test_survey_log`: add below CAP; count/at ordering; overflow evicts oldest; clear.

### `IrrigationWeb::buildSurvey` + `parseSurveyStart` (`IrrigationWebApi.*`)

```cpp
// build: JSON array of survey points for the panel.
size_t buildSurvey(const ::SurveyPoint *pts, size_t n, char *buf, size_t cap);
// parse: portal "start survey" request → interval/timeout/coord.
struct SurveyStartReq { uint16_t intervalS; uint16_t timeoutS; int32_t latE7, lonE7; bool hasCoord; };
bool parseSurveyStart(const char *json, size_t n, SurveyStartReq &out);
```

`SurveyPoint` is defined at global scope in `SurveyLog.h`; `SurveyStartReq` lives in namespace
`IrrigationWeb`. `IrrigationWebApi` includes `SurveyLog.h` (pure header). `buildSurvey`
emits `{no,role,vbat,fw,lat,lon,hasCoord,snr,rssi,idadeS}` per row. `parseSurveyStart` reads
`intervalS`/`timeoutS` (defaults applied by caller when absent) and optional `lat`/`lon`
(`hasCoord=false` when either missing). Added to existing suite `test_irrigation_webapi`.

## Glue — `IrrigationModule` (thin, behind the existing module seam)

- Own `SurveyBeacon surveyBeacon;` and `SurveyLog surveyLog;` (harmless on all roles; only the
  gateway populates the log, only station/SERVICO drive the beacon).
- **Tick** (existing periodic path): `if (surveyBeacon.tick(millis())) emitSurveyBeacon();`
- `emitSurveyBeacon()` — allocate `PING_SURVEY`, `kind=2`, fill `role/configEpoch/vbatCentiV/
  fwVersion` from `settings`+`batteryCentiV()`, `latE7/lonE7` from the beacon (or 0), broadcast.
  If `role==SERVICO`, `setServiceFlag(...)` (mirrors `svcEmitProbe`).
- `handlePingSurvey` — new branch **before** the reply logic:
  ```cpp
  if (req.kind == 2) {                    // BEACON (§8.5)
      if (role == GATEWAY) {
          SurveyPoint sp{ mp.from, req.role, req.vbatCentiV, req.fwVersion,
                          req.latE7, req.lonE7, /*hasCoord*/ (req.latE7||req.lonE7),
                          (int8_t)(mp.rx_snr*4), (int16_t)mp.rx_rssi, uptimeSeconds() };
          surveyLog.add(sp);
      }
      return;                              // never reply to a beacon
  }
  ```
- Accessors (driven by endpoints):
  - `bool portalStartSurvey(const IrrigationWeb::SurveyStartReq &r);` — validates role can beacon,
    calls `surveyBeacon.start(...)`. Shared by node portal and SERVICO portal.
  - `void portalStopSurvey();`
  - `size_t buildSurveyLog(char *buf, size_t cap);` — `IrrigationWeb::buildSurvey(surveyLog…)`.
  - `void clearSurveyLog();`

## Endpoints (CI-only; excluded from native, gated as the existing ones)

- **Gateway panel** (`IrrigationWebEndpoints.cpp`, `#if !MESHTASTIC_EXCLUDE_WEBSERVER`, role==GATEWAY):
  `GET /api/irrigation/survey` → `buildSurveyLog`; `POST /api/irrigation/survey/clear` → `clearSurveyLog`.
  Mirror `hAudit`/`hExport`.
- **Node captive portal §7.2** (`IrrigationPortalEndpoints.cpp`): `POST /api/portal/survey/start`
  (body → `parseSurveyStart` → `portalStartSurvey`), `POST /api/portal/survey/stop`.
- **SERVICO portal §11.8** (`ServicePortalEndpoints.cpp`, gated role==SERVICO):
  `POST /api/portal/service/survey/start` + `/stop` → same module accessors.

## Frontend

- **Gateway panel** `data/irrigacao/` (`index.html`/`app.js`): new "Cobertura" tab. Poll
  `GET /api/irrigation/survey` (~2 s), render table: nó, role, coord (lat/lon or "—"), SNR, RSSI,
  idade. "Limpar" button → `survey/clear`.
- **Node captive portal §7.2 UI** and **SERVICO portal** `data/irrigacao/portal/`: a "Survey /
  Cobertura" control — intervalo (s), timeout (min), lat/lon (optional), Iniciar/Parar buttons →
  the `survey/start|stop` endpoints. (Exact §7.2 HTML asset path is resolved in the plan; the node
  portal served by `PortalAp` may embed its own HTML rather than reuse `data/irrigacao/portal/`.)

## Defaults

- Beacon interval: **5 s**. Timeout: **5 min**. Both portal-editable. (Normal power; ~60 beacons
  per default run — acceptable install-time airtime.)

## Testing

- New suites: `test/test_survey_beacon/`, `test/test_survey_log/`. Bump `test/native-suite-count`
  60 → 62.
- Extend `test/test_irrigation_webapi` with `buildSurvey` + `parseSurveyStart` cases.
- Full native suite GREEN (Docker) at the end. Endpoints/frontend never compile natively (CI/bench).
- **Bench 2+ nodes** validates the radio path (beacon → gateway log → panel), SNR/RSSI, expiry.

## Risks / notes

- `mp.rx_rssi` type/range — confirm field on `meshtastic_MeshPacket` at glue time (store as `int16_t`).
- `uptimeSeconds()` source on the gateway (no RTC) — reuse whatever `ServiceController::logService`
  used for its timestamp, or `millis()/1000`.
- Airtime: kind=2 does **not** trigger replies, unlike kind=0 — this is the key reason beacons use a
  distinct kind rather than reusing the PROBE.
- `hasCoord` heuristic `(latE7||lonE7)` treats exact 0/0 as "no coord"; acceptable (0,0 is ocean).
