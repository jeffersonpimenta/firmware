# Horário (gateway) + relógio nos nós — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adicionar a página **Horário** ao painel do gateway (fonte de hora NTP/manual, fuso, sync por dispositivo) e um **relógio na página principal** dos portais de nó.

**Architecture:** Builders/parsers JSON puros em `IrrigationWebApi`/`PortalApi` (testáveis nativo), cola de estado em `IrrigationModule`, rotas HTTP em `IrrigationWebEndpoints`, UI vanilla-JS em `data/irrigacao`. NTP já existe (`WiFiAPClient`); expomos gatilho + timestamp. Sem mudança de ABI de estação.

**Tech Stack:** C++ (Arduino/ESP32, Unity para testes nativos), `esp32_https_server`, JS vanilla. Testes nativos rodam via Docker (`./bin/run-tests.sh` — ver memória "native-tests-need-docker-on-windows").

**Spec:** `docs/superpowers/specs/2026-08-06-irrigation-horario-clock-design.md`

---

## Notas de execução (ler antes)

- **Rodar 1 teste nativo:** `./bin/run-tests.sh -f test_irrigation_webapi` (ou `test_portal_api`). Exit 0 = GREEN. Em Windows, o runner usa o container `Dockerfile.test` + volume `pio-build`.
- **Formatar:** `trunk fmt` antes de cada commit de C++.
- Handlers HTTP e cola de módulo **não** têm teste nativo (dependem de hardware/servidor). Verificação = compilar (`pio run -e <env>` não é obrigatório no plano; confie na revisão) + teste manual no harness MCP. Para essas tasks, o "teste" é a inspeção do diff + os testes de builder já cobrindo o JSON.

---

## File Structure

**Modificar:**
- `src/modules/irrigation/IrrigationWebApi.h` — `TimeStatusCtx`, `buildTimeStatus`, `parseTimeSet`, `parseTimezone`, tabela `TZ_PRESETS` + helpers.
- `src/modules/irrigation/IrrigationWebApi.cpp` — implementações acima.
- `src/modules/irrigation/PortalApi.h` — 2 campos de relógio em `NodeStateCtx`.
- `src/modules/irrigation/PortalApi.cpp` — emitir os 2 campos em `buildNodeState`.
- `src/modules/irrigation/IrrigationModule.h` — declarar `gwBuildTimeStatus/gwSetManualTime/gwSetTimezone/gwSyncNtpNow`.
- `src/modules/irrigation/IrrigationModule.cpp` — implementar; preencher relógio em `portalFillNodeState`.
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — handlers `hTime`(GET/POST), `hTimezone`, `hTimeSync` + registro.
- `src/mesh/wifi/WiFiAPClient.h` / `.cpp` — `triggerNtpUpdate()` e `ntpLastRunMs()`.
- `data/irrigacao/index.html` — `<span id="timeBadge">` no header.
- `data/irrigacao/app.js` — `renderHorario`, `TZ_PRESETS`, badge, entradas em `renderMais`/`RENDER`/`SECTION_LABELS`.
- `data/irrigacao/portal/app.js` — linha "Relógio" em `renderNode`.
- `data/irrigacao/mock.js` — campos `nowEpoch`/`hasTime` no mock de node-state (para preview offline).
- `test/test_irrigation_webapi/test_main.cpp` — testes de `buildTimeStatus`/parsers/TZ.
- `test/test_portal_api/test_main.cpp` — teste do relógio no node-state.

---

## Task 1: `buildTimeStatus` + tabela de fusos (builder puro)

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

- [ ] **Step 1: Escrever o teste que falha**

Adicionar em `test/test_irrigation_webapi/test_main.cpp` (antes do `void setup()`/lista de RUN_TEST):

```cpp
static void test_buildTimeStatus_ntp()
{
    TimeStatusCtx c = {};
    c.nowEpoch = 1754500320;
    c.quality = 3; // RTCQualityNTP
    c.ntpServer = "pool.ntp.org";
    c.lastSyncS = 120;
    c.tz = "<-03>3";
    c.staUp = true;
    char buf[512];
    size_t n = buildTimeStatus(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"nowEpoch\":1754500320"));
    TEST_ASSERT_TRUE(contains(buf, "\"hasRtc\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"source\":\"ntp\""));
    TEST_ASSERT_TRUE(contains(buf, "\"ntpServer\":\"pool.ntp.org\""));
    TEST_ASSERT_TRUE(contains(buf, "\"lastSyncS\":120"));
    TEST_ASSERT_TRUE(contains(buf, "\"tz\":\"<-03>3\""));
    TEST_ASSERT_TRUE(contains(buf, "\"tzLabel\":\"America/Sao_Paulo\""));
    TEST_ASSERT_TRUE(contains(buf, "\"staUp\":true"));
}

static void test_buildTimeStatus_manual_and_none()
{
    TimeStatusCtx c = {};
    c.nowEpoch = 1754500320;
    c.quality = 1; // RTCQualityDevice -> manual
    c.lastSyncS = -1;
    c.tz = "GMT0";
    char buf[512];
    TEST_ASSERT_GREATER_THAN(0, buildTimeStatus(c, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(contains(buf, "\"source\":\"manual\""));
    TEST_ASSERT_TRUE(contains(buf, "\"lastSyncS\":-1"));
    TEST_ASSERT_TRUE(contains(buf, "\"tzLabel\":\"UTC\""));

    TimeStatusCtx z = {};
    z.nowEpoch = 0; z.quality = 0; z.tz = "";
    char buf2[512];
    TEST_ASSERT_GREATER_THAN(0, buildTimeStatus(z, buf2, sizeof(buf2)));
    TEST_ASSERT_TRUE(contains(buf2, "\"source\":\"none\""));
    TEST_ASSERT_TRUE(contains(buf2, "\"hasRtc\":false"));
    TEST_ASSERT_TRUE(contains(buf2, "\"tzLabel\":\"Personalizado\""));
}

static void test_tzPresets_lookup()
{
    TEST_ASSERT_TRUE(tzIsValidPreset("<-03>3"));
    TEST_ASSERT_TRUE(tzIsValidPreset("GMT0"));
    TEST_ASSERT_FALSE(tzIsValidPreset("Europe/Paris"));
    TEST_ASSERT_EQUAL_STRING("America/Manaus", tzLabelFor("<-04>4"));
    TEST_ASSERT_EQUAL_STRING("Personalizado", tzLabelFor("bogus"));
}
```

Registrar na função `setup()` (junto aos outros `RUN_TEST`):

```cpp
    RUN_TEST(test_buildTimeStatus_ntp);
    RUN_TEST(test_buildTimeStatus_manual_and_none);
    RUN_TEST(test_tzPresets_lookup);
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FAIL de compilação — `buildTimeStatus`/`TimeStatusCtx`/`tzIsValidPreset`/`tzLabelFor` não declarados.

- [ ] **Step 3: Declarar no header**

Em `src/modules/irrigation/IrrigationWebApi.h`, logo após `size_t buildOverview(...)` (linha ~64), adicionar:

```cpp
// --- Fase 8b: Horário (relógio do gateway) ---
struct TzPreset {
    const char *label; // IANA-like exibido na UI
    const char *posix; // string gravada em config.device.tzdef
};
extern const TzPreset TZ_PRESETS[];
extern const size_t TZ_PRESETS_COUNT;
// "Personalizado" se posix não bate nenhum preset (ou é vazio).
const char *tzLabelFor(const char *posix);
bool tzIsValidPreset(const char *posix);

struct TimeStatusCtx {
    uint32_t nowEpoch = 0;      // 0 = sem relógio válido
    int quality = 0;            // getRTCQuality() cru (NTP=3, GPS=2, Device=1, None=0)
    const char *ntpServer = ""; // config.network.ntp_server
    int32_t lastSyncS = -1;     // segundos desde o último NTP set; -1 = nunca
    const char *tz = "";        // config.device.tzdef
    bool staUp = false;         // WiFi STA conectado
};
size_t buildTimeStatus(const TimeStatusCtx &ctx, char *buf, size_t cap);
```

- [ ] **Step 4: Implementar no .cpp**

Em `src/modules/irrigation/IrrigationWebApi.cpp`, após `buildOverview` (linha ~84), adicionar:

```cpp
const TzPreset TZ_PRESETS[] = {
    {"America/Sao_Paulo", "<-03>3"},
    {"America/Manaus", "<-04>4"},
    {"America/Rio_Branco", "<-05>5"},
    {"America/Noronha", "<-02>2"},
    {"UTC", "GMT0"},
};
const size_t TZ_PRESETS_COUNT = sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]);

const char *tzLabelFor(const char *posix)
{
    if (posix)
        for (size_t i = 0; i < TZ_PRESETS_COUNT; i++)
            if (strcmp(posix, TZ_PRESETS[i].posix) == 0)
                return TZ_PRESETS[i].label;
    return "Personalizado";
}
bool tzIsValidPreset(const char *posix)
{
    if (!posix) return false;
    for (size_t i = 0; i < TZ_PRESETS_COUNT; i++)
        if (strcmp(posix, TZ_PRESETS[i].posix) == 0)
            return true;
    return false;
}

size_t buildTimeStatus(const TimeStatusCtx &ctx, char *buf, size_t cap)
{
    const char *source = ctx.quality >= 3 ? "ntp" : (ctx.nowEpoch != 0 ? "manual" : "none");
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyNum("nowEpoch", (int64_t)ctx.nowEpoch);
    w.keyBool("hasRtc", ctx.nowEpoch != 0);
    w.keyStr("source", source);
    w.keyNum("quality", ctx.quality);
    w.keyStr("ntpServer", ctx.ntpServer ? ctx.ntpServer : "");
    w.keyNum("lastSyncS", ctx.lastSyncS);
    w.keyStr("tz", ctx.tz ? ctx.tz : "");
    w.keyStr("tzLabel", tzLabelFor(ctx.tz));
    w.keyBool("staUp", ctx.staUp);
    w.endObject();
    return w.done();
}
```

`<cstring>` já é incluído no topo do .cpp (`#include <cstring>`).

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: PASS (todos, incluindo os 3 novos).

- [ ] **Step 6: Formatar + commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.cpp src/modules/irrigation/IrrigationWebApi.h
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): buildTimeStatus + tabela de fusos (fase 8b)"
```

---

## Task 2: parsers `parseTimeSet` / `parseTimezone`

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

- [ ] **Step 1: Escrever o teste que falha**

Adicionar em `test/test_irrigation_webapi/test_main.cpp`:

```cpp
static void test_parseTimeSet_ok()
{
    const char *j = "{\"epoch\":1754500320}";
    uint32_t epoch = 0;
    ParseResult r = parseTimeSet(j, strlen(j), epoch);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT32(1754500320u, epoch);
}
static void test_parseTimeSet_rejectsImplausible()
{
    const char *j = "{\"epoch\":100}"; // antes de 2020 -> inválido
    uint32_t epoch = 0;
    ParseResult r = parseTimeSet(j, strlen(j), epoch);
    TEST_ASSERT_FALSE(r.ok);
}
static void test_parseTimezone_ok_and_reject()
{
    char tz[40] = {0};
    const char *ok = "{\"tz\":\"<-03>3\"}";
    ParseResult r = parseTimezone(ok, strlen(ok), tz, sizeof(tz));
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("<-03>3", tz);

    char tz2[40] = {0};
    const char *bad = "{\"tz\":\"Europe/Paris\"}"; // fora do preset
    ParseResult r2 = parseTimezone(bad, strlen(bad), tz2, sizeof(tz2));
    TEST_ASSERT_FALSE(r2.ok);
}
```

Registrar em `setup()`:

```cpp
    RUN_TEST(test_parseTimeSet_ok);
    RUN_TEST(test_parseTimeSet_rejectsImplausible);
    RUN_TEST(test_parseTimezone_ok_and_reject);
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FAIL de compilação — `parseTimeSet`/`parseTimezone` não declarados.

- [ ] **Step 3: Declarar no header**

Em `IrrigationWebApi.h`, após `buildTimeStatus`:

```cpp
// epoch plausível: >= 1_600_000_000 (2020-09) e <= 4_102_444_800 (2100).
ParseResult parseTimeSet(const char *json, size_t len, uint32_t &epochOut);
// Copia tz para out (validado contra TZ_PRESETS). Rejeita fora do preset.
ParseResult parseTimezone(const char *json, size_t len, char *out, size_t outCap);
```

`ParseResult` e `JsonReader` já vêm de `IrrigationProtocol.h` (incluído no header). Confirmar que `ParseResult`/`JsonReader` são visíveis neste header — os parsers existentes (`parseZoneUpsert` etc.) já os usam aqui.

- [ ] **Step 4: Implementar no .cpp**

Em `IrrigationWebApi.cpp`, após `buildTimeStatus`:

```cpp
ParseResult parseTimeSet(const char *json, size_t len, uint32_t &epochOut)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t epoch = 0;
    if (!rd.getInt("epoch", epoch)) { r.fail("epoch ausente"); return r; }
    if (epoch < 1600000000LL || epoch > 4102444800LL) { r.fail("epoch fora de range"); return r; }
    epochOut = (uint32_t)epoch;
    return r;
}

ParseResult parseTimezone(const char *json, size_t len, char *out, size_t outCap)
{
    ParseResult r;
    JsonReader rd(json, len);
    char tz[40] = {0};
    if (!rd.getStr("tz", tz, sizeof(tz)) || tz[0] == '\0') { r.fail("tz ausente"); return r; }
    if (!tzIsValidPreset(tz)) { r.fail("fuso desconhecido"); return r; }
    strncpy(out, tz, outCap - 1);
    out[outCap - 1] = '\0';
    return r;
}
```

Verificar a assinatura de `JsonReader::getInt/getStr` no header de protocolo (mesmo uso de `parsePulse`/`parseNetCommand`). Se `getInt` for `bool getInt(const char*, int64_t&)` — é o caso — o código acima está correto.

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: PASS.

- [ ] **Step 6: Formatar + commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.cpp src/modules/irrigation/IrrigationWebApi.h
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): parseTimeSet/parseTimezone (fase 8b)"
```

---

## Task 3: relógio no node-state (builder puro)

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h`
- Modify: `src/modules/irrigation/PortalApi.cpp`
- Test: `test/test_portal_api/test_main.cpp`

- [ ] **Step 1: Escrever o teste que falha**

Adicionar em `test/test_portal_api/test_main.cpp` (mesmo estilo Unity; usar helper `contains` local — se não existir no arquivo, copiar `static bool contains(const char*h,const char*n){return strstr(h,n)!=nullptr;}`):

```cpp
static void test_buildNodeState_emits_clock()
{
    NodeStateCtx c = {};
    c.role = 0;
    c.name = "Est 1";
    c.nowEpoch = 1754500320;
    c.hasTime = true;
    char buf[512];
    size_t n = buildNodeState(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"nowEpoch\":1754500320"));
    TEST_ASSERT_TRUE(contains(buf, "\"hasTime\":true"));
}
```

Registrar o `RUN_TEST(test_buildNodeState_emits_clock);` na lista de testes de `setup()` desse arquivo.

- [ ] **Step 2: Rodar e ver falhar**

Run: `./bin/run-tests.sh -f test_portal_api`
Expected: FAIL — `NodeStateCtx` não tem `nowEpoch`/`hasTime`.

- [ ] **Step 3: Adicionar campos ao ctx**

Em `src/modules/irrigation/PortalApi.h`, dentro de `struct NodeStateCtx`, após `uint32_t uptimeS = 0;` (linha 27):

```cpp
    uint32_t nowEpoch = 0; // getValidTime local (0 = sem relógio) — display-only (fase 8b)
    bool hasTime = false;
```

- [ ] **Step 4: Emitir no builder**

Em `src/modules/irrigation/PortalApi.cpp`, dentro de `buildNodeState`, após `w.keyNum("uptimeS", (int64_t)ctx.uptimeS);` (linha 25):

```cpp
    w.keyNum("nowEpoch", (int64_t)ctx.nowEpoch);
    w.keyBool("hasTime", ctx.hasTime);
```

- [ ] **Step 5: Rodar e ver passar**

Run: `./bin/run-tests.sh -f test_portal_api`
Expected: PASS.

- [ ] **Step 6: Formatar + commit**

```bash
trunk fmt src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp
git add src/modules/irrigation/PortalApi.h src/modules/irrigation/PortalApi.cpp test/test_portal_api/test_main.cpp
git commit -m "feat(irrigation): relógio (nowEpoch/hasTime) no node-state (fase 8b)"
```

---

## Task 4: gatilho NTP + timestamp em WiFiAPClient

**Files:**
- Modify: `src/mesh/wifi/WiFiAPClient.h`
- Modify: `src/mesh/wifi/WiFiAPClient.cpp`

Sem teste nativo (depende de WiFi/hardware). Verificação = inspeção do diff.

- [ ] **Step 1: Declarar helpers no header**

Em `src/mesh/wifi/WiFiAPClient.h`, adicionar (perto das outras declarações `extern`/funções livres):

```cpp
// Fase 8b (irrigação): força um novo fetch NTP no próximo tick (zera o throttle).
// No-op se DISABLE_NTP. Seguro chamar quando WiFi não está conectado (fetch só corre com STA up).
void triggerNtpUpdate();
// millis() do último NTP set bem-sucedido; 0 se nunca sincronizou.
unsigned long ntpLastRunMs();
```

- [ ] **Step 2: Implementar no .cpp**

Em `src/mesh/wifi/WiFiAPClient.cpp`, no fim do arquivo (fora de qualquer função), adicionar. `lastrun_ntp` já é global no arquivo (declarado `unsigned long lastrun_ntp = 0;`, linha ~60):

```cpp
void triggerNtpUpdate()
{
#ifndef DISABLE_NTP
    lastrun_ntp = 0; // próximo serialAndWifiHandler() refaz o NTP se WiFi conectado
#endif
}

unsigned long ntpLastRunMs()
{
#ifndef DISABLE_NTP
    return lastrun_ntp;
#else
    return 0;
#endif
}
```

Nota: `lastrun_ntp` só recebe `millis()` **após** um NTP bem-sucedido (linha ~280, `lastrun_ntp = millis();`). Então `0` = nunca. `triggerNtpUpdate()` reusa esse mesmo campo para forçar o próximo ciclo.

- [ ] **Step 3: Commit**

```bash
trunk fmt src/mesh/wifi/WiFiAPClient.h src/mesh/wifi/WiFiAPClient.cpp
git add src/mesh/wifi/WiFiAPClient.h src/mesh/wifi/WiFiAPClient.cpp
git commit -m "feat(wifi): triggerNtpUpdate/ntpLastRunMs p/ painel de irrigação (fase 8b)"
```

---

## Task 5: cola de módulo (time status / set manual / fuso / sync)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`
- Modify: `src/modules/irrigation/IrrigationModule.cpp`

Sem teste nativo. Verificação = inspeção do diff (os builders já testados fazem o JSON).

- [ ] **Step 1: Declarar métodos no header**

Em `src/modules/irrigation/IrrigationModule.h`, junto aos outros `gw*` (após `size_t gwBuildAlerts(...)`, linha ~128):

```cpp
    // Fase 8b: página Horário. gwBuildTimeStatus monta o TimeStatusCtx + serializa.
    size_t gwBuildTimeStatus(char *buf, size_t cap);
    bool gwSetManualTime(uint32_t epoch);   // perhapsSetRTC(Device, force) — true se aplicou
    bool gwSetTimezone(const char *posix);  // grava config.device.tzdef + setenv + persiste
    bool gwSyncNtpNow();                     // dispara NTP; false se WiFi STA down
```

- [ ] **Step 2: Implementar no .cpp**

No topo de `src/modules/irrigation/IrrigationModule.cpp`, garantir os includes (o bloco WiFi da fase 8a já inclui `<WiFi.h>` e `WiFiAPClient.h` **dentro** de `#if defined(ARCH_ESP32)` na linha ~3408; a hora precisa deles fora desse guard). Adicionar perto dos includes do topo:

```cpp
#include "gps/RTC.h" // getValidTime/getRTCQuality/perhapsSetRTC
```

Para `config.device.tzdef` / `config.network.ntp_server` / `service->reloadConfig`: `configuration.h`/`NodeDB.h` já são visíveis no módulo (usa `config` e `service` em outros pontos — confirmar; a seção WiFi 8a já usa `config`). Adicionar a implementação perto de `computeLocalSecs` (linha ~2346):

```cpp
size_t IrrigationModule::gwBuildTimeStatus(char *buf, size_t cap)
{
    IrrigationWeb::TimeStatusCtx c = {};
    c.nowEpoch = getValidTime(RTCQualityDevice, true);
    c.quality = (int)getRTCQuality();
#if defined(ARCH_ESP32)
    c.staUp = WiFi.isConnected();
    unsigned long last = ntpLastRunMs();
    c.lastSyncS = (last != 0) ? (int32_t)((millis() - last) / 1000UL) : -1;
#else
    c.staUp = false;
    c.lastSyncS = -1;
#endif
    c.ntpServer = config.network.ntp_server[0] ? config.network.ntp_server : "pool.ntp.org";
    c.tz = config.device.tzdef; // "" se não definido
    return IrrigationWeb::buildTimeStatus(c, buf, cap);
}

bool IrrigationModule::gwSetManualTime(uint32_t epoch)
{
    if (epoch < 1600000000u) return false;
    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    perhapsSetRTC(RTCQualityDevice, &tv, /*forceUpdate=*/true);
    LOG_INFO("Irrigation GW: hora definida manualmente (epoch=%u)", epoch);
    return true;
}

bool IrrigationModule::gwSetTimezone(const char *posix)
{
    if (!posix || !IrrigationWeb::tzIsValidPreset(posix)) return false;
    strncpy(config.device.tzdef, posix, sizeof(config.device.tzdef) - 1);
    config.device.tzdef[sizeof(config.device.tzdef) - 1] = '\0';
    setenv("TZ", config.device.tzdef, 1);
    tzset();
    if (service) service->reloadConfig(SEGMENT_CONFIG); // persiste + reaplica config
    LOG_INFO("Irrigation GW: fuso ajustado (%s)", config.device.tzdef);
    return true;
}

bool IrrigationModule::gwSyncNtpNow()
{
#if defined(ARCH_ESP32)
    if (!WiFi.isConnected()) return false;
    triggerNtpUpdate();
    return true;
#else
    return false;
#endif
}
```

Incluir `#include "mesh/wifi/WiFiAPClient.h"` no topo do .cpp **fora** do guard (as funções `triggerNtpUpdate`/`ntpLastRunMs` são globais; o header não puxa `<WiFi.h>`). Se der conflito de include ordem, envolver as chamadas `WiFi.*` já estão sob `#if defined(ARCH_ESP32)`.

- [ ] **Step 3: Preencher relógio no portalFillNodeState**

Em `portalFillNodeState` (linha ~2658), após `out.uptimeS = millis() / 1000;`:

```cpp
    out.nowEpoch = getValidTime(RTCQualityDevice, true);
    out.hasTime = out.nowEpoch != 0;
```

- [ ] **Step 4: Commit**

```bash
trunk fmt src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): cola de Horário no módulo + relógio no node-state (fase 8b)"
```

---

## Task 6: endpoints HTTP `/time`, `/timezone`, `/time/sync`

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp`

Sem teste nativo (servidor HTTP). Verificação = inspeção do diff, espelhando `hMaint`/`hOverview`.

- [ ] **Step 1: Adicionar os handlers**

Em `src/modules/irrigation/IrrigationWebEndpoints.cpp`, antes de `void registerIrrigationHandlers(...)` (linha ~897):

```cpp
// GET /api/irrigation/time — estado do relógio do gateway
static void hTime(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    char buf[512];
    if (!irrigationModule->gwBuildTimeStatus(buf, sizeof(buf))) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// POST /api/irrigation/time — set manual { epoch }
static void hTimeSet(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    uint32_t epoch = 0;
    ParseResult pr = parseTimeSet(body, nb, epoch);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->gwSetManualTime(epoch)) {
        sendJson(res, "{\"ok\":false,\"reason\":\"epoch inválido\"}", 400);
        return;
    }
    char buf[512];
    irrigationModule->gwBuildTimeStatus(buf, sizeof(buf));
    sendJson(res, buf);
}

// POST /api/irrigation/timezone — { tz } (preset POSIX)
static void hTimezone(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    char tz[40] = {0};
    ParseResult pr = parseTimezone(body, nb, tz, sizeof(tz));
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->gwSetTimezone(tz)) {
        sendJson(res, "{\"ok\":false,\"reason\":\"fuso desconhecido\"}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// POST /api/irrigation/time/sync — dispara NTP agora (só com WiFi STA)
static void hTimeSync(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    if (!irrigationModule->gwSyncNtpNow()) {
        sendJson(res, "{\"ok\":false,\"reason\":\"sem WiFi\"}", 409);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}
```

- [ ] **Step 2: Registrar as rotas**

Dentro de `registerIrrigationHandlers`, antes do `}` de fecho (após a linha do survey, ~935):

```cpp
    // Fase 8b: Horário (relógio do gateway)
    server->registerNode(new ResourceNode("/api/irrigation/time", "GET", &hTime));
    server->registerNode(new ResourceNode("/api/irrigation/time", "POST", &hTimeSet));
    server->registerNode(new ResourceNode("/api/irrigation/timezone", "POST", &hTimezone));
    server->registerNode(new ResourceNode("/api/irrigation/time/sync", "POST", &hTimeSync));
```

- [ ] **Step 3: Commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebEndpoints.cpp
git add src/modules/irrigation/IrrigationWebEndpoints.cpp
git commit -m "feat(irrigation): endpoints /time, /timezone, /time/sync (fase 8b)"
```

---

## Task 7: página Horário no painel do gateway (JS)

**Files:**
- Modify: `data/irrigacao/index.html`
- Modify: `data/irrigacao/app.js`

Sem teste automatizado (JS de dispositivo). Verificação = abrir o painel (ou preview) e conferir a tela.

- [ ] **Step 1: Badge de relógio no header**

Em `data/irrigacao/index.html`, trocar a linha 15 (`<span id="syncChip" ...>`) por um wrapper com badge:

```html
      <div class="hdr-right">
        <span id="syncChip" class="chip green">—</span>
        <span id="timeBadge" class="chip amber timebadge hidden" role="button" tabindex="0">—</span>
      </div>
```

- [ ] **Step 2: Presets de fuso + renderHorario no app.js**

Em `data/irrigacao/app.js`, antes de `// ===== Roteamento =====` (linha ~2625), adicionar:

```javascript
// ===== Horário (relógio do gateway) — endpoints /api/irrigation/time* =====
const TZ_PRESETS = [
  ['America/Sao_Paulo', '<-03>3'],
  ['America/Manaus', '<-04>4'],
  ['America/Rio_Branco', '<-05>5'],
  ['America/Noronha', '<-02>2'],
  ['UTC', 'GMT0'],
];

function fmtEpochLocal(epoch) {
  if (!epoch) return '—';
  const d = new Date(epoch * 1000);
  const p = (n) => String(n).padStart(2, '0');
  return `${p(d.getDate())}/${p(d.getMonth() + 1)}/${d.getFullYear()} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

async function renderHorario() {
  const t = (await getJson('/time').catch(() => ({}))) || {};
  const stations = (await getJson('/stations').catch(() => [])) || [];
  const isNtp = t.source === 'ntp';
  const isManual = t.source === 'manual' || t.source === 'none';
  const lastSync =
    t.lastSyncS == null || t.lastSyncS < 0
      ? 'nunca'
      : t.lastSyncS === 0
      ? 'agora mesmo'
      : fmtSince(t.lastSyncS);

  const tzRows = TZ_PRESETS.map(
    ([label, posix]) =>
      `<button class="tzrow${t.tz === posix ? ' sel' : ''}" data-tz="${esc(posix)}">${esc(label)}</button>`
  ).join('');

  const devRows = (Array.isArray(stations) ? stations : [])
    .map((s) => {
      const [cls, lbl] = stationSyncLabel(s.sync);
      const nm = s.name ? esc(s.name) : nodeHex(s.node);
      return `<div class="fp-kv"><span class="k">${nm}</span><span class="chip ${cls}">${esc(lbl)}</span></div>`;
    })
    .join('') || '<div class="sub">Nenhuma estação conhecida.</div>';

  view.innerHTML = `
    <div class="card">
      <div class="sens-hdr"><span class="name">Relógio do gateway</span></div>
      <div class="sub">Agora: ${fmtEpochLocal(t.nowEpoch)} · ${esc(t.tzLabel || '—')}</div>
    </div>
    <div class="card stack">
      <div class="fp-lbl">Fonte de hora</div>
      <div class="fp-seg" id="srcSeg">
        <button data-src="ntp" class="${isNtp ? 'sel' : ''}">NTP (automática)</button>
        <button data-src="manual" class="${!isNtp ? 'sel' : ''}">Manual</button>
      </div>
      <div id="srcBody"></div>
      <span id="timeMsg" class="sub"></span>
    </div>
    <div class="card">
      <div class="sens-hdr"><span class="name">Fuso horário</span></div>
      <div class="tzlist">${tzRows}</div>
    </div>
    <div class="card">
      <div class="sens-hdr"><span class="name">Sincronização por dispositivo</span></div>
      <div class="sub">Epoch de config de cada estação — reflete se recebeu o horário/config mais recente.</div>
      ${devRows}
    </div>`;

  const srcBody = view.querySelector('#srcBody');
  const msg = view.querySelector('#timeMsg');
  function paintSource(src) {
    if (src === 'ntp') {
      srcBody.innerHTML = `
        <div class="fp-kv"><span class="k">Servidor</span><span class="v">${esc(t.ntpServer || '—')}</span></div>
        <div class="fp-kv"><span class="k">Última sincronização</span><span class="v">${esc(lastSync)}</span></div>
        ${t.staUp ? '<button class="btn ghost sm" id="syncNow">Sincronizar agora</button>' : '<div class="sub">Sem WiFi — NTP indisponível.</div>'}`;
      const sn = srcBody.querySelector('#syncNow');
      if (sn)
        sn.addEventListener('click', async () => {
          const r = await postJson('/time/sync', {});
          msg.textContent = r.ok ? 'Sincronização NTP disparada.' : 'Falha: ' + (r.body.reason || 'sem WiFi');
          setTimeout(() => renderHorario().catch(() => {}), 1500);
        });
    } else {
      const nowIso = t.nowEpoch ? new Date(t.nowEpoch * 1000) : new Date();
      const p = (n) => String(n).padStart(2, '0');
      const dv = `${nowIso.getFullYear()}-${p(nowIso.getMonth() + 1)}-${p(nowIso.getDate())}`;
      const tv = `${p(nowIso.getHours())}:${p(nowIso.getMinutes())}`;
      srcBody.innerHTML = `
        <label>Data <input type="date" id="mDate" value="${dv}"></label>
        <label>Hora <input type="time" id="mTime" value="${tv}"></label>
        <button class="btn solid sm" id="mSet">Definir data e hora</button>`;
      srcBody.querySelector('#mSet').addEventListener('click', async () => {
        const d = srcBody.querySelector('#mDate').value;
        const h = srcBody.querySelector('#mTime').value;
        if (!d || !h) { msg.textContent = 'Preencha data e hora.'; return; }
        const epoch = Math.floor(new Date(`${d}T${h}:00`).getTime() / 1000);
        const r = await postJson('/time', { epoch });
        msg.textContent = r.ok ? 'Data e hora aplicadas.' : 'Falha: ' + (r.body.reason || 'erro');
        setTimeout(() => renderHorario().catch(() => {}), 1500);
      });
    }
  }
  paintSource(isNtp ? 'ntp' : 'manual');
  view.querySelectorAll('#srcSeg button').forEach((b) =>
    b.addEventListener('click', () => {
      view.querySelectorAll('#srcSeg button').forEach((x) => x.classList.toggle('sel', x === b));
      paintSource(b.dataset.src);
    })
  );
  view.querySelectorAll('.tzrow').forEach((b) =>
    b.addEventListener('click', async () => {
      const r = await postJson('/timezone', { tz: b.dataset.tz });
      msg.textContent = r.ok ? 'Fuso atualizado.' : 'Falha ao definir fuso.';
      setTimeout(() => renderHorario().catch(() => {}), 800);
    })
  );
}
```

- [ ] **Step 2b: Registrar no menu/roteador**

Em `data/irrigacao/app.js`:

Em `renderMais()` `items` (linha ~2174, antes de `['sistema', ...]`):
```javascript
    ['horario', 'Horário', 'Fonte de hora, fuso e sincronização dos nós'],
```
Em `RENDER` (linha ~2626): `horario: renderHorario,`
Em `SECTION_LABELS` (linha ~2646): adicionar `horario: 'Horário',`

- [ ] **Step 3: Badge no header — atualizar no overview**

Em `renderOverview()`, logo após setar o `syncChip` (linha ~99), adicionar:

```javascript
  const tb = document.getElementById('timeBadge');
  if (tb) {
    if (ov.hasRtc) {
      tb.textContent = fmtEpochLocal(getJsonNowEpoch(ov));
      tb.classList.remove('hidden');
    } else {
      tb.classList.add('hidden');
    }
  }
```

Como o `/overview` **não** traz `nowEpoch`, simplificar: o badge usa o relógio do **cliente** (o celular tem hora real), coerente com `nextFireMinutes`. Substituir a linha do textContent por:

```javascript
      const now = new Date();
      const p = (n) => String(n).padStart(2, '0');
      tb.textContent = `${p(now.getHours())}:${p(now.getMinutes())}`;
```

E remover a referência a `getJsonNowEpoch`. Adicionar uma única vez o listener de clique (fora do render, junto ao setup de tabs no fim do arquivo, ~2713):

```javascript
const timeBadgeEl = document.getElementById('timeBadge');
if (timeBadgeEl) timeBadgeEl.addEventListener('click', () => showSub('horario'));
```

- [ ] **Step 4: CSS mínimo**

Em `data/irrigacao/style.css`, acrescentar ao fim:

```css
.hdr-right { display: flex; align-items: center; gap: 8px; }
.timebadge { cursor: pointer; }
.tzlist { display: flex; flex-direction: column; gap: 6px; }
.tzrow { text-align: left; border: 1px solid var(--line, #ddd); background: #fff; padding: 10px 12px; border-radius: 10px; font-weight: 600; cursor: pointer; }
.tzrow.sel { border-color: var(--green, #2e7d5b); color: var(--green, #2e7d5b); background: rgba(46,125,91,0.08); }
```

Confirmar os nomes de var CSS existentes no `style.css` e ajustar (usar as vars já definidas no arquivo; se não houver, manter os fallbacks acima).

- [ ] **Step 5: Verificação manual**

Abrir o painel do gateway (ou preview com `mock.js`). Ir em Mais → Horário. Conferir: 4 cards; alternar NTP/Manual; lista de fusos realça o atual; badge de hora no topo abre a página.

- [ ] **Step 6: Commit**

```bash
trunk fmt data/irrigacao/app.js data/irrigacao/index.html data/irrigacao/style.css
git add data/irrigacao/app.js data/irrigacao/index.html data/irrigacao/style.css
git commit -m "feat(irrigation): painel — página Horário + badge de relógio no header (fase 8b)"
```

---

## Task 8: relógio na página principal dos nós (JS)

**Files:**
- Modify: `data/irrigacao/portal/app.js`
- Modify: `data/irrigacao/mock.js`

- [ ] **Step 1: Helper de formatação + linha no card**

Em `data/irrigacao/portal/app.js`, adicionar um helper perto de `fmtUptime` (linha ~7):

```javascript
function fmtClock(epoch, has) {
  if (!has || !epoch) return "sem relógio";
  const d = new Date(epoch * 1000);
  const p = (n) => String(n).padStart(2, "0");
  return `${p(d.getDate())}/${p(d.getMonth() + 1)}/${d.getFullYear()} ${p(d.getHours())}:${p(d.getMinutes())}`;
}
```

- [ ] **Step 2: Mostrar no ramo repetidor**

Em `renderNode()`, no ramo `if (s.role === ROLE_REPETIDOR)`, trocar a linha `<div class="muted" ...>${sync}</div>` (linha ~44) por:

```javascript
      <div class="fp-stat" style="margin-top:8px;"><div class="lbl">Relógio</div><div class="val sm">${fmtClock(s.nowEpoch, s.hasTime)}</div></div>
      <div class="muted" style="font-size:12px;margin-top:12px;">${sync}</div>`;
```

- [ ] **Step 3: Mostrar no ramo estação**

No ramo padrão (estação), após a linha `<div class="muted" ...>${sync}${s.safeMode ...}</div>` (linha ~59), inserir antes das válvulas:

```javascript
    <div class="fp-stat" style="margin-top:8px;"><div class="lbl">Relógio</div><div class="val sm">${fmtClock(s.nowEpoch, s.hasTime)}</div></div>
```

- [ ] **Step 4: Campos no mock (preview offline)**

Em `data/irrigacao/mock.js`, no objeto de node-state (procurar onde tem `numValves`/`uptimeS`), acrescentar:

```javascript
    nowEpoch: Math.floor(Date.now() / 1000),
    hasTime: true,
```

- [ ] **Step 5: Verificação manual**

Abrir `/irrigacao/portal/` (estação e repetidor via mock). Card "Este nó" mostra a linha "Relógio".

- [ ] **Step 6: Commit**

```bash
trunk fmt data/irrigacao/portal/app.js data/irrigacao/mock.js
git add data/irrigacao/portal/app.js data/irrigacao/mock.js
git commit -m "feat(irrigation): portal — relógio na página principal do nó (fase 8b)"
```

---

## Task 9: suíte verde + atualização de memória

- [ ] **Step 1: Rodar a suíte inteira**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN). Contagem deve subir vs. baseline (novos casos em `test_irrigation_webapi` +6, `test_portal_api` +1).

- [ ] **Step 2: Se algo quebrar** — usar superpowers:systematic-debugging; não silenciar teste.

- [ ] **Step 3: Atualizar memória do projeto**

Criar `memory/irrigation-phase8b-status.md` (frontmatter `type: project`) resumindo: página Horário no gateway + relógio nos nós; NTP reaproveitado (triggerNtpUpdate); fuso via presets→tzdef; sem ABI de estação; nova contagem da suíte; follow-ups (badge usa hora do cliente; repetidores fora do card de sync). Adicionar linha em `MEMORY.md`.

---

## Self-review (autor do plano)

- **Cobertura da spec:**
  - GET /time → Task 1+5. POST /time (manual) → Task 2+5. POST /timezone → Task 2+5. POST /time/sync → Task 4+5. ✓
  - Presets de fuso → Task 1 (C++) + Task 7 (JS, mesma tabela). ✓
  - Página Horário (4 cards) → Task 7. ✓
  - Badge no header → Task 7. ✓
  - Relógio no nó (display-only) → Task 3 (backend) + Task 8 (JS). ✓
  - "Fonte reflete realidade, sem suprimir NTP" → `source` derivado de `quality` (Task 1); sem toque no fluxo NTP além de gatilho aditivo (Task 4). ✓
- **Placeholders:** nenhum "TODO/TBD"; todo passo de código tem código.
- **Consistência de tipos:** `TimeStatusCtx` (nowEpoch/quality/ntpServer/lastSyncS/tz/staUp) idêntico entre Task 1 (def), Task 5 (uso). `parseTimeSet(...,uint32_t&)` e `parseTimezone(...,char*,size_t)` idênticos Task 2↔5↔6. `nowEpoch`/`hasTime` idênticos Task 3↔5↔8. Rotas idênticas Task 5↔6↔7. ✓
- **A confirmar na impl (riscos da spec):** chamada exata de persistência (`service->reloadConfig(SEGMENT_CONFIG)` — confirmado no padrão do repo); `getRTCQuality()` acessível via `gps/RTC.h` (confirmado); repetidores podem não aparecer no `/stations` → card de sync lista só estações na v1 (aceito na spec).
