# Fase 6a — Sensores, GPO, tamper e mini-log na estação — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Estação com entradas de sensor calibradas reportadas no heartbeat (+ envio antecipado), GPO fim-a-fim (comando → nível → estado no ACK/heartbeat), alerta de tamper com janela de manutenção, mini-log de auditoria persistente consultável pelo portal, e coordenadas locais (settings ABI v4).

**Architecture:** Camadas da 5a/5b. (A) Puro host-testado — `IrrigationSettings` v4, bloco de sensores no heartbeat + `EV_TAMPER`, `SensorSampler`, `GpoController`, `AuditLog` (compartilhado com a 6b). (B) Cola no `IrrigationModule` (+ endpoints ESP32-only do portal). (C) Frontend estático do portal.

**Tech Stack:** C++17, PlatformIO Unity (suíte nativa), `esp32_https_server`, LittleFS, HTML/vanilla-JS.

> Spec de design: `docs/superpowers/specs/2026-07-20-irrigacao-fase6a-estacao-design.md`.

---

## Global Constraints

- Teto absoluto 120 min (`ValveController::MAX_OPEN_SECONDS`) para válvulas; GPO **biestável** (`durationS==0`) é exceção prevista na spec §8.11 — sem teto, exige `confirm` na UI.
- Camada A pura: nunca `Arduino.h`/WiFi/HTTP/`FSCom`. Persistência só na camada B.
- Payload rádio ≤ `IrrigationProto::MAX_PAYLOAD` (200 B). Sem bump de `IrrigationProto::VERSION`.
- Cola HTTP sob `#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER` onde tocar `PortalAp`/endpoints (padrão 5b).
- Testes: `./bin/run-tests.sh` GREEN. Suíte nova bumpa `test/native-suite-count` no MESMO commit.
- `trunk fmt` roda no CI (não instalado localmente).
- Comentários em português; identificadores no estilo dos vizinhos.

## Como rodar uma suíte nativa (esta máquina precisa de Docker)

```powershell
docker run --rm -v "${PWD}:/firmware" -v pio-build:/firmware/.pio -v pio-cache:/root/.platformio -e HOME=/root -w /firmware --user root mesh-test ./bin/run-tests.sh -f <suite>
```

(Pré-req uma vez: `docker build -f Dockerfile.test -t mesh-test .`.) A corrida filtrada compila o firmware inteiro — erros de cola/módulo aparecem como erro de build. Exit codes: 0 GREEN, 1 RED, 2 AMBER, 3 FILTERED.

## File Structure

Criar:

- `src/modules/irrigation/SensorSampler.h` / `.cpp` — leitura/condicionamento dos 4 slots (puro).
- `src/modules/irrigation/GpoController.h` / `.cpp` — saídas de nível biestável/temporizada (puro).
- `src/modules/irrigation/AuditLog.h` / `.cpp` — ring de auditoria serializável (puro; 6b reusa).
- `test/test_sensor_sampler/test_main.cpp`, `test/test_gpo_controller/test_main.cpp`, `test/test_audit_log/test_main.cpp`.

Modificar:

- `src/modules/irrigation/IrrigationSettings.h` / `.cpp` — ABI v4 (Task 1).
- `src/modules/irrigation/IrrigationProtocol.h` / `.cpp` — bloco de sensores no heartbeat, `EV_TAMPER` (Task 2).
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — GPO, sampler, tamper, auditoria (Tasks 6–8).
- `src/modules/irrigation/PortalApi.h` / `.cpp` — builders/parsers novos (Task 9).
- `src/modules/irrigation/IrrigationPortalEndpoints.cpp` — rotas novas (Task 10).
- `data/irrigacao/portal/index.html`, `app.js` — UI (Task 11).
- `test/test_irrigation_config/test_main.cpp`, `test/test_irrigation_protocol/test_main.cpp`, `test/test_portal_api/test_main.cpp` — extensões.
- `test/native-suite-count` — 46 → 47 (Task 3) → 48 (Task 4) → 49 (Task 5).
- `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` — Fase 5 concluída; Fase 6 dividida (Task 12).

---

# MARCO 1 — Camada A pura

## Task 1: `IrrigationSettings` ABI v4

Blob 52 → **128 bytes**. Campos v3 preservados como prefixo (offsets inalterados); apêndice v4:

```
 52  pinsGpo[2]           (2)
 54  pinTamper            (1)
 55  hwFlags              (1)   bit0 = tamper ativo-baixo
 56  latE7                (4)   int32, grau ×1e7; 0/0 = não preenchido
 60  lonE7                (4)
 64  sensores[4] × 16     (64)
Total = 128
```

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h`
- Modify: `src/modules/irrigation/IrrigationSettings.cpp`
- Test: `test/test_irrigation_config/test_main.cpp` (suíte existente — sem bump)

- [ ] **Step 1: Teste de migração v3→v4 que falha**

Em `test/test_irrigation_config/test_main.cpp`, adicionar (e registrar no `main`):

```cpp
static void test_migrate_v3_to_v4()
{
    // Blob v3 sintético: struct atual "rebaixada" — monta 52 bytes com version=3.
    IrrigationSettings v4;
    v4.numValves = 3;
    v4.pinBtn = 0;
    v4.configEpoch = 9;
    uint8_t raw[52];
    memcpy(raw, &v4, 52); // prefixo v3 == primeiros 52 bytes do v4
    uint16_t ver3 = 3;
    memcpy(raw + 4, &ver3, 2);

    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, 52, out));
    TEST_ASSERT_EQUAL_UINT16(4, out.version);
    TEST_ASSERT_EQUAL_UINT8(3, out.numValves);
    TEST_ASSERT_EQUAL_UINT32(9, out.configEpoch);
    // Campos novos em default
    TEST_ASSERT_EQUAL_INT8(-1, out.pinsGpo[0]);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinsGpo[1]);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinTamper);
    TEST_ASSERT_EQUAL_INT32(0, out.latE7);
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL_INT8(-1, out.sensores[i].pino);
}

static void test_v4_roundtrip_and_size()
{
    TEST_ASSERT_EQUAL_size_t(128, sizeof(IrrigationSettings));
    IrrigationSettings s;
    s.sensores[1] = {36, 1, 0, 30, 0, 300, 3800, 0, 1000, 1, 0}; // analógico bar
    s.pinsGpo[0] = 27;
    s.latE7 = -221234560;
    uint8_t raw[sizeof(IrrigationSettings)];
    memcpy(raw, &s, sizeof(s));
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_INT8(36, out.sensores[1].pino);
    TEST_ASSERT_EQUAL_INT16(1000, out.sensores[1].engMax);
    TEST_ASSERT_EQUAL_INT32(-221234560, out.latE7);
}
```

- [ ] **Step 2: Rodar e ver falhar** — `... -f test_irrigation_config` → RED (campos inexistentes: erro de compilação).

- [ ] **Step 3: Implementar v4**

`IrrigationSettings.h` — dentro da struct, após `pad2`:

```cpp
    // v4 (Fase 6a):
    static constexpr uint8_t MAX_GPO = 2;
    static constexpr uint8_t MAX_SENSORS = 4;

    struct SensorSlot {
        int8_t pino = -1;        // -1 = slot vazio
        uint8_t tipo = 0;        // 0 = digital, 1 = analógico
        uint8_t flags = 0;       // bit0 = ativo-baixo (digital)
        uint8_t amostragemS = 0; // analógico; 0 → default 30
        uint16_t debounceMs = 0; // digital; 0 → default 200
        uint16_t adcMin = 0;     // calibração 2 pontos (analógico)
        uint16_t adcMax = 4095;
        int16_t engMin = 0;      // centi-unidades (1000 = 10,00)
        int16_t engMax = 0;
        uint8_t unidade = 0;     // 0 raw, 1 bar, 2 %, 3 m, 4 °C
        uint8_t pad = 0;
    };

    int8_t pinsGpo[MAX_GPO] = {-1, -1}; // saídas de nível (§8.11)
    int8_t pinTamper = -1;              // §8.12; -1 desativa
    uint8_t hwFlags = 0;                // bit0 = tamper ativo-baixo
    int32_t latE7 = 0;                  // coordenadas locais ×1e-7 (§8.8)
    int32_t lonE7 = 0;
    SensorSlot sensores[MAX_SENSORS];
```

Atualizar `version = 4` no default, o comentário de layout do topo, e:

```cpp
static constexpr size_t IRRIGATION_SETTINGS_V3_SIZE = 52;
static_assert(sizeof(IrrigationSettings::SensorSlot) == 16, "SensorSlot é ABI on-disk");
static_assert(sizeof(IrrigationSettings) == 128, "on-disk settings format is ABI-dependent; bump version on layout change");
```

(Manter `IRRIGATION_SETTINGS_V1_SIZE = 40`.)

`IrrigationSettings.cpp` — `migrateIrrigationSettings` vira cadeia v4→v1:

```cpp
    if (version == 4) {
        if (n != sizeof(IrrigationSettings))
            return false;
        memcpy(&out, raw, sizeof(out));
        return true;
    }
    if (version == 3 || version == 2) {
        if (n != IRRIGATION_SETTINGS_V3_SIZE)
            return false;
        IrrigationSettings s; // defaults v4 nos campos novos
        memcpy(&s, raw, IRRIGATION_SETTINGS_V3_SIZE); // layout v2/v3 é prefixo do v4
        s.version = 4;
        if (version == 2) { // bytes de v3 eram padding no v2
            s.pinBtn = -1;
            s.pinLed = -1;
            s.pad2 = 0;
        }
        out = s;
        return true;
    }
    if (version == 1) {
        if (n != IRRIGATION_SETTINGS_V1_SIZE)
            return false;
        IrrigationSettings s;
        memcpy(&s, raw, IRRIGATION_SETTINGS_V1_SIZE);
        s.version = 4;
        out = s;
        return true;
    }
    return false;
```

Em `loadIrrigationSettings`: buffer `raw[sizeof(IrrigationSettings)]` já acompanha; trocar a detecção de upgrade para `bool migrated = (n != sizeof(IrrigationSettings)) || (rawVersion < 4);`.

- [ ] **Step 4: Rodar e ver passar** — `... -f test_irrigation_config` → GREEN.

- [ ] **Step 5: Commit** — `feat(irrigation): settings ABI v4 (sensores, GPO, tamper, coords)`

## Task 2: Protocolo — bloco de sensores no heartbeat + `EV_TAMPER`

Trailing opcional após `configEpoch`: `count(u8) + count × { id(u8), tipo(u8), valueCenti(i16) }`. Payload antigo (sem bloco) decodifica com `sensorCount = 0`. `count > 4` = payload inválido.

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h`
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp`
- Test: `test/test_irrigation_protocol/test_main.cpp` (suíte existente)

- [ ] **Step 1: Testes que falham**

```cpp
static void test_heartbeat_sensor_block_roundtrip()
{
    Heartbeat hb = {};
    hb.valveStates = 0x3;
    hb.configEpoch = 7;
    hb.sensorCount = 2;
    hb.sensors[0] = {0, 1, 152};  // analógico 1,52
    hb.sensors[1] = {1, 0, 100};  // digital ativo
    uint8_t buf[64];
    size_t n = encodeHeartbeat(buf, sizeof(buf), 1, hb);
    TEST_ASSERT_TRUE(n > 0);
    Heartbeat out;
    TEST_ASSERT_TRUE(decodeHeartbeat(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(2, out.sensorCount);
    TEST_ASSERT_EQUAL_INT16(152, out.sensors[0].valueCenti);
    TEST_ASSERT_EQUAL_UINT8(0, out.sensors[1].tipo);
}

static void test_heartbeat_legacy_payload_decodes_zero_sensors()
{
    Heartbeat hb = {};
    hb.sensorCount = 0;
    uint8_t buf[64];
    size_t n = encodeHeartbeat(buf, sizeof(buf), 1, hb);
    // count=0 emite só o byte de contagem; truncar também o byte simula payload v. anterior
    Heartbeat out;
    TEST_ASSERT_TRUE(decodeHeartbeat(buf, n - 1, out));
    TEST_ASSERT_EQUAL_UINT8(0, out.sensorCount);
}

static void test_heartbeat_sensor_count_overflow_rejected()
{
    Heartbeat hb = {};
    uint8_t buf[64];
    size_t n = encodeHeartbeat(buf, sizeof(buf), 1, hb);
    buf[n - 1] = 5; // count > HB_MAX_SENSORS
    Heartbeat out;
    TEST_ASSERT_FALSE(decodeHeartbeat(buf, n, out));
}
```

E round-trip do `EV_TAMPER`: `Evento{EV_TAMPER, 1}` encode/decode.

- [ ] **Step 2: Rodar e ver falhar** — `... -f test_irrigation_protocol` → RED.

- [ ] **Step 3: Implementar**

`IrrigationProtocol.h`:

```cpp
constexpr uint8_t HB_MAX_SENSORS = 4;

struct SensorReading {
    uint8_t id;
    uint8_t tipo;       // 0 digital, 1 analógico
    int16_t valueCenti; // digital: 0/100
};
```

Em `Heartbeat`, após `configEpoch`: `uint8_t sensorCount = 0; SensorReading sensors[HB_MAX_SENSORS] = {};`
Em `EventCode`: `EV_TAMPER = 6` (arg: 1 = abriu, 0 = fechou).

`IrrigationProtocol.cpp` — `encodeHeartbeat`, após `w.u32(m.configEpoch)`:

```cpp
    uint8_t cnt = m.sensorCount > HB_MAX_SENSORS ? HB_MAX_SENSORS : m.sensorCount;
    w.u8(cnt);
    for (uint8_t i = 0; i < cnt; i++) {
        w.u8(m.sensors[i].id);
        w.u8(m.sensors[i].tipo);
        w.i16(m.sensors[i].valueCenti);
    }
```

`decodeHeartbeat`, após `out.configEpoch = r.u32()`:

```cpp
    if (!r.ok)
        return false;
    // Bloco de sensores é opcional (payloads de firmware anterior não o têm)
    out.sensorCount = 0;
    if (r.pos >= len)
        return true;
    uint8_t cnt = r.u8();
    if (cnt > HB_MAX_SENSORS)
        return false;
    for (uint8_t i = 0; i < cnt; i++) {
        out.sensors[i].id = r.u8();
        out.sensors[i].tipo = r.u8();
        out.sensors[i].valueCenti = r.i16();
    }
    out.sensorCount = r.ok ? cnt : 0;
    return r.ok;
```

- [ ] **Step 4: Rodar e ver passar** → GREEN.
- [ ] **Step 5: Commit** — `feat(irrigation): heartbeat sensor block + EV_TAMPER`

## Task 3: `SensorSampler`

Puro; leitor injetado. Digital: polaridade + debounce. Analógico: amostragem periódica + calibração linear com clamp. Mudança significativa (digital mudou; analógico saiu da banda de 2 % do span vs último reportado) arma heartbeat antecipado, rate-limited a 30 s desde o último reporte.

**Files:**
- Create: `src/modules/irrigation/SensorSampler.h`, `.cpp`
- Create: `test/test_sensor_sampler/test_main.cpp`
- Modify: `test/native-suite-count` → 47

- [ ] **Step 1: Header (interface fixa)**

```cpp
#pragma once
#include "modules/irrigation/IrrigationProtocol.h" // SensorReading
#include "modules/irrigation/IrrigationSettings.h"
#include <stddef.h>
#include <stdint.h>

// Leitor injetado: implementação Arduino fica no módulo; testes usam mock.
class ISensorReader
{
  public:
    virtual ~ISensorReader() = default;
    virtual uint16_t readAdc(int8_t pin) = 0;
    virtual bool readLevel(int8_t pin) = 0; // nível bruto, sem polaridade
};

class SensorSampler
{
  public:
    static constexpr uint16_t DEFAULT_DEBOUNCE_MS = 200;
    static constexpr uint8_t DEFAULT_AMOSTRAGEM_S = 30;
    static constexpr uint32_t EARLY_HB_MIN_INTERVAL_MS = 30u * 1000u;
    static constexpr uint8_t HYST_PCT = 2; // % do span de engenharia

    void configure(const IrrigationSettings &s);
    void tick(uint32_t nowMs, ISensorReader &rd);
    // Preenche leituras válidas; devolve quantas (0..MAX_SENSORS).
    size_t readings(IrrigationProto::SensorReading out[IrrigationSettings::MAX_SENSORS]) const;
    bool earlyHeartbeatDue(uint32_t nowMs) const;
    void noteReported(uint32_t nowMs); // chamar após TODO envio de heartbeat

  private:
    struct SlotState {
        bool valid = false;
        int16_t valueCenti = 0;
        // digital
        bool rawStable = false;
        bool lastRaw = false;
        uint32_t rawSinceMs = 0;
        bool everSampled = false;
        // analógico
        uint32_t lastSampleMs = 0;
        // referência do último valor reportado (para histerese)
        bool everReported = false;
        int16_t reportedCenti = 0;
    };
    IrrigationSettings cfg;
    SlotState st[IrrigationSettings::MAX_SENSORS];
    bool pending = false;
    uint32_t lastReportMs = 0;

    int16_t calibrate(const IrrigationSettings::SensorSlot &sl, uint16_t adc) const;
};
```

- [ ] **Step 2: Testes que falham** (`test/test_sensor_sampler/test_main.cpp`, molde das suítes vizinhas: `setUp/tearDown`, `main` com `UNITY_BEGIN/RUN_TEST/UNITY_END`)

```cpp
struct FakeReader : ISensorReader {
    uint16_t adc[40] = {};
    bool level[40] = {};
    uint16_t readAdc(int8_t pin) override { return adc[pin]; }
    bool readLevel(int8_t pin) override { return level[pin]; }
};

static IrrigationSettings cfgAnalog()
{
    IrrigationSettings s;
    s.sensores[0] = {36, 1, 0, 30, 0, 1000, 3000, 0, 1000, 1, 0}; // 1000..3000 adc → 0..10,00 bar
    return s;
}

static IrrigationSettings cfgDigital()
{
    IrrigationSettings s;
    s.sensores[0] = {39, 0, 1, 0, 200, 0, 4095, 0, 0, 0, 0}; // ativo-baixo, debounce 200 ms
    return s;
}

static void test_analog_calibration_and_clamp()
{
    SensorSampler sm;
    sm.configure(cfgAnalog());
    FakeReader rd;
    rd.adc[36] = 2000; // meio da faixa → 5,00 bar
    sm.tick(0, rd);
    IrrigationProto::SensorReading out[4];
    TEST_ASSERT_EQUAL_size_t(1, sm.readings(out));
    TEST_ASSERT_EQUAL_INT16(500, out[0].valueCenti);
    rd.adc[36] = 100; // abaixo de adcMin → clamp em engMin
    sm.tick(31000, rd);
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(0, out[0].valueCenti);
}

static void test_analog_sampling_period()
{
    SensorSampler sm;
    sm.configure(cfgAnalog());
    FakeReader rd;
    rd.adc[36] = 2000;
    sm.tick(0, rd);
    rd.adc[36] = 3000;
    sm.tick(10000, rd); // < 30 s: não re-amostra
    IrrigationProto::SensorReading out[4];
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(500, out[0].valueCenti);
    sm.tick(30000, rd); // período vencido
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(1000, out[0].valueCenti);
}

static void test_digital_debounce_and_polarity()
{
    SensorSampler sm;
    sm.configure(cfgDigital());
    FakeReader rd;
    rd.level[39] = true; // ativo-baixo: nível alto = inativo
    sm.tick(0, rd);
    sm.tick(300, rd); // estável 300 ms
    IrrigationProto::SensorReading out[4];
    TEST_ASSERT_EQUAL_size_t(1, sm.readings(out));
    TEST_ASSERT_EQUAL_INT16(0, out[0].valueCenti); // inativo
    rd.level[39] = false; // vai a ativo
    sm.tick(400, rd);     // ainda dentro do debounce
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(0, out[0].valueCenti);
    sm.tick(700, rd); // 300 ms estável > 200 ms
    sm.readings(out);
    TEST_ASSERT_EQUAL_INT16(100, out[0].valueCenti); // ativo
}

static void test_early_heartbeat_hysteresis_and_ratelimit()
{
    // amostragemS = 1 para a linha do tempo curta do teste
    IrrigationSettings cfg = cfgAnalog();
    cfg.sensores[0].amostragemS = 1;
    SensorSampler sm;
    sm.configure(cfg);
    FakeReader rd;
    rd.adc[36] = 2000; // 5,00 bar
    sm.tick(0, rd);
    sm.noteReported(0); // heartbeat de boot reportou 5,00
    rd.adc[36] = 2010;  // ~5,05: dentro da banda de 2 % (span 1000 → banda 20 centi)
    sm.tick(1000, rd);
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(1001)); // sem mudança significativa
    rd.adc[36] = 2500; // 7,50: fora da banda → pendente
    sm.tick(2000, rd);
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(2001));  // rate-limit: < 30 s do último reporte
    TEST_ASSERT_TRUE(sm.earlyHeartbeatDue(30000));  // 30 s decorridos → dispara
    sm.noteReported(30000);
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(30001)); // pendência limpa após reporte
}

static void test_digital_change_arms_early_heartbeat()
{
    SensorSampler sm;
    sm.configure(cfgDigital());
    FakeReader rd;
    rd.level[39] = true;
    sm.tick(0, rd);
    sm.tick(300, rd);
    sm.noteReported(300);
    rd.level[39] = false;
    sm.tick(400, rd);
    sm.tick(700, rd); // estado estável mudou
    TEST_ASSERT_FALSE(sm.earlyHeartbeatDue(701)); // < 30 s do último reporte
    TEST_ASSERT_TRUE(sm.earlyHeartbeatDue(300 + 30000));
}
```

- [ ] **Step 3: Rodar e ver falhar** → RED (arquivo não existe).

- [ ] **Step 4: Implementar `SensorSampler.cpp`**

```cpp
#include "SensorSampler.h"

void SensorSampler::configure(const IrrigationSettings &s)
{
    cfg = s;
    for (auto &sl : st)
        sl = SlotState{};
    pending = false;
    lastReportMs = 0;
}

int16_t SensorSampler::calibrate(const IrrigationSettings::SensorSlot &sl, uint16_t adc) const
{
    if (sl.adcMax <= sl.adcMin)
        return sl.engMin; // calibração degenerada: valor fixo, sem div/0
    if (adc <= sl.adcMin)
        return sl.engMin;
    if (adc >= sl.adcMax)
        return sl.engMax;
    int32_t num = (int32_t)(adc - sl.adcMin) * (sl.engMax - sl.engMin);
    return (int16_t)(sl.engMin + num / (sl.adcMax - sl.adcMin));
}

void SensorSampler::tick(uint32_t nowMs, ISensorReader &rd)
{
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        const auto &sl = cfg.sensores[i];
        auto &s = st[i];
        if (sl.pino < 0)
            continue;
        if (sl.tipo == 1) { // analógico
            uint32_t periodMs = (sl.amostragemS ? sl.amostragemS : DEFAULT_AMOSTRAGEM_S) * 1000u;
            if (s.valid && nowMs - s.lastSampleMs < periodMs)
                continue;
            s.valueCenti = calibrate(sl, rd.readAdc(sl.pino));
            s.lastSampleMs = nowMs;
            s.valid = true;
            // Histerese: banda = 2 % do span (mínimo 1 centi)
            int32_t span = (int32_t)sl.engMax - sl.engMin;
            if (span < 0)
                span = -span;
            int32_t band = span * HYST_PCT / 100;
            if (band < 1)
                band = 1;
            int32_t delta = (int32_t)s.valueCenti - s.reportedCenti;
            if (delta < 0)
                delta = -delta;
            if (s.everReported && delta > band)
                pending = true;
        } else { // digital
            bool raw = rd.readLevel(sl.pino);
            bool active = (sl.flags & 1) ? !raw : raw;
            uint16_t db = sl.debounceMs ? sl.debounceMs : DEFAULT_DEBOUNCE_MS;
            if (!s.everSampled || active != s.lastRaw) {
                s.lastRaw = active;
                s.rawSinceMs = nowMs;
                s.everSampled = true;
                continue; // aguarda estabilidade
            }
            if (nowMs - s.rawSinceMs < db)
                continue;
            int16_t v = active ? 100 : 0;
            if (s.valid && v != s.valueCenti && s.everReported)
                pending = true;
            s.valueCenti = v;
            s.valid = true;
        }
    }
}

size_t SensorSampler::readings(IrrigationProto::SensorReading out[IrrigationSettings::MAX_SENSORS]) const
{
    size_t n = 0;
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        if (cfg.sensores[i].pino < 0 || !st[i].valid)
            continue;
        out[n++] = {i, cfg.sensores[i].tipo, st[i].valueCenti};
    }
    return n;
}

bool SensorSampler::earlyHeartbeatDue(uint32_t nowMs) const
{
    return pending && (nowMs - lastReportMs >= EARLY_HB_MIN_INTERVAL_MS);
}

void SensorSampler::noteReported(uint32_t nowMs)
{
    pending = false;
    lastReportMs = nowMs;
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        if (!st[i].valid)
            continue;
        st[i].reportedCenti = st[i].valueCenti;
        st[i].everReported = true;
    }
}
```

Ajustar detalhes até os testes do Step 2 passarem (a semântica dos testes manda).

- [ ] **Step 5: Bump `test/native-suite-count` 46 → 47 (mesmo commit).**
- [ ] **Step 6: Rodar `-f test_sensor_sampler` → GREEN.**
- [ ] **Step 7: Commit** — `feat(irrigation): SensorSampler (calibração, debounce, HB antecipado)`

## Task 4: `GpoController`

Padrão do `ValveController`, saída de **nível**. `durationS==0` = biestável (sem timer); `>0` = desliga sozinho. `allOff()` para modo seguro/factory reset.

**Files:**
- Create: `src/modules/irrigation/GpoController.h`, `.cpp`
- Create: `test/test_gpo_controller/test_main.cpp`
- Modify: `test/native-suite-count` → 48

- [ ] **Step 1: Header**

```cpp
#pragma once
#include <stdint.h>

// Saída de nível (relé/MOSFET). Implementação GPIO fica no módulo; testes usam mock.
class IGpoDriver
{
  public:
    virtual ~IGpoDriver() = default;
    virtual void set(uint8_t index, bool on) = 0;
};

class GpoController
{
  public:
    static constexpr uint8_t MAX_GPO = 2;

    enum class Result : uint8_t { OK, INVALID_ID };

    GpoController(IGpoDriver &driver, uint8_t numGpos);

    // action: 1 = ligar, 0 = desligar. durationS==0 ao ligar = biestável (§8.11).
    Result command(uint8_t id, uint8_t action, uint16_t durationS, uint32_t nowMs);
    void tick(uint32_t nowMs); // desliga temporizados expirados
    void allOff();             // modo seguro / factory reset
    bool isOn(uint8_t id) const { return id < numGpos && slots[id].on; }
    uint8_t states() const;
    void setNumGpos(uint8_t n); // reconfig runtime: encolher desliga as removidas

  private:
    struct Slot {
        bool on = false;
        uint32_t offAtMs = 0; // 0 = biestável
    };
    IGpoDriver &driver;
    uint8_t numGpos;
    Slot slots[MAX_GPO];
};
```

- [ ] **Step 2: Testes que falham**

```cpp
struct FakeGpoDriver : IGpoDriver {
    bool level[GpoController::MAX_GPO] = {};
    int setCount = 0;
    void set(uint8_t i, bool on) override { level[i] = on; setCount++; }
};

static void test_bistable_stays_on()
{
    FakeGpoDriver d;
    GpoController g(d, 2);
    TEST_ASSERT_EQUAL(GpoController::Result::OK, g.command(0, 1, 0, 1000));
    TEST_ASSERT_TRUE(d.level[0]);
    g.tick(1000 + 24u * 3600u * 1000u); // 24 h depois: continua ligado
    TEST_ASSERT_TRUE(g.isOn(0));
    TEST_ASSERT_EQUAL_UINT8(0x1, g.states());
    g.command(0, 0, 0, 0);
    TEST_ASSERT_FALSE(d.level[0]);
}

static void test_timed_auto_off()
{
    FakeGpoDriver d;
    GpoController g(d, 2);
    g.command(1, 1, 60, 0); // 60 s
    g.tick(59999);
    TEST_ASSERT_TRUE(g.isOn(1));
    g.tick(60000);
    TEST_ASSERT_FALSE(g.isOn(1)); // fail-safe local
    TEST_ASSERT_FALSE(d.level[1]);
}

static void test_invalid_id_and_alloff()
{
    FakeGpoDriver d;
    GpoController g(d, 1);
    TEST_ASSERT_EQUAL(GpoController::Result::INVALID_ID, g.command(1, 1, 0, 0));
    g.command(0, 1, 0, 0);
    g.allOff();
    TEST_ASSERT_EQUAL_UINT8(0, g.states());
}

static void test_shrink_turns_off_removed()
{
    FakeGpoDriver d;
    GpoController g(d, 2);
    g.command(1, 1, 0, 0);
    g.setNumGpos(1);
    TEST_ASSERT_FALSE(d.level[1]);
    TEST_ASSERT_EQUAL_UINT8(0, g.states());
}
```

- [ ] **Step 3: Rodar → RED.**

- [ ] **Step 4: Implementar `GpoController.cpp`** (direto: `command` valida id < numGpos, seta driver + slot, `offAtMs = durationS ? nowMs + durationS * 1000u : 0`; `tick` desliga `on && offAtMs && nowMs >= offAtMs`; `allOff`/`setNumGpos` iteram chamando `driver.set(i, false)`).

- [ ] **Step 5: Bump suite-count → 48 (mesmo commit). Rodar `-f test_gpo_controller` → GREEN. Commit** — `feat(irrigation): GpoController (biestável + temporizado fail-safe)`

## Task 5: `AuditLog`

Ring de registros de 16 B com storage do chamador (estação: 100; gateway 6b: maior). Serialização com magic+CRC (padrão `ProgramScheduler`), reusando `IrrigationProto::crc32`.

**Files:**
- Create: `src/modules/irrigation/AuditLog.h`, `.cpp`
- Create: `test/test_audit_log/test_main.cpp`
- Modify: `test/native-suite-count` → 49

- [ ] **Step 1: Header**

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

// §8.9 — origens/ações/resultados do log de auditoria.
enum class AuditOrigin : uint8_t {
    SISTEMA = 0, CRONOGRAMA, PAINEL, PORTAL_CAMPO, BOTAO_FISICO,
    ENTRADA_FISICA, INTERTRAVAMENTO, FAILSAFE_TIMER, SERVICO
};
enum class AuditAction : uint8_t {
    ABRIR = 0, FECHAR, PULSO, GPO_ON, GPO_OFF, PAREAR, FACTORY_RESET,
    CONFIG_EPOCH, SAFE_MODE_IN, SAFE_MODE_OUT, TAMPER, REBOOT,
    HIBERNA_IN, HIBERNA_OUT
};
enum class AuditResult : uint8_t { OK = 0, NACK, TIMEOUT };

struct AuditRecord {
    uint32_t tsSecs = 0; // epoch local; 0 = sem RTC no momento
    uint8_t origin = 0;  // AuditOrigin
    uint8_t action = 0;  // AuditAction
    uint8_t target = 0;  // válvula/GPO/zona/código conforme a ação
    uint8_t result = 0;  // AuditResult (NACK: motivo vai em `target` quando aplicável)
    uint32_t node = 0;   // ator remoto (0 = local)
    uint32_t seq = 0;    // seq da mensagem de rádio (0 = ação local)
};

// Ring somente-append. Storage é do chamador (estação: 100; gateway: maior).
class AuditLog
{
  public:
    static constexpr uint32_t MAGIC = 0x49414C31; // "IAL1"

    AuditLog(AuditRecord *storage, size_t cap) : buf(storage), cap(cap) {}

    void append(const AuditRecord &r);
    size_t size() const { return num; }
    // i = 0 é o MAIS RECENTE.
    const AuditRecord &at(size_t i) const;
    size_t serialize(uint8_t *out, size_t outCap) const; // 0 = não coube
    bool deserialize(const uint8_t *in, size_t n);       // mantém os mais recentes se cap menor
    void clear();

  private:
    AuditRecord *buf;
    size_t cap;
    size_t writeIdx = 0;
    size_t num = 0;
};
```

- [ ] **Step 2: Testes que falham** — wrap (cap 4, append 6, `at(0)` = último, `size()==4`), ordem mais-recente-primeiro, round-trip serialize/deserialize, CRC corrompido → `deserialize` false e log intacto, deserialize em log de cap menor mantém os mais recentes, `clear()`.

```cpp
static AuditRecord rec(uint32_t ts) { AuditRecord r; r.tsSecs = ts; r.action = (uint8_t)AuditAction::ABRIR; return r; }

static void test_ring_wrap_and_order()
{
    AuditRecord store[4];
    AuditLog log(store, 4);
    for (uint32_t i = 1; i <= 6; i++)
        log.append(rec(i));
    TEST_ASSERT_EQUAL_size_t(4, log.size());
    TEST_ASSERT_EQUAL_UINT32(6, log.at(0).tsSecs); // mais recente primeiro
    TEST_ASSERT_EQUAL_UINT32(3, log.at(3).tsSecs);
}

static void test_serialize_roundtrip_and_crc()
{
    AuditRecord a[8], b[8];
    AuditLog la(a, 8), lb(b, 8);
    la.append(rec(10));
    la.append(rec(20));
    uint8_t buf[256];
    size_t n = la.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(lb.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(2, lb.size());
    TEST_ASSERT_EQUAL_UINT32(20, lb.at(0).tsSecs);
    buf[n - 1] ^= 0xff; // corrompe CRC
    TEST_ASSERT_FALSE(lb.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(2, lb.size()); // intacto
}

static void test_deserialize_smaller_cap_keeps_recent()
{
    AuditRecord a[8], c[2];
    AuditLog la(a, 8), lc(c, 2);
    for (uint32_t i = 1; i <= 5; i++)
        la.append(rec(i));
    uint8_t buf[256];
    size_t n = la.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(lc.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(2, lc.size());
    TEST_ASSERT_EQUAL_UINT32(5, lc.at(0).tsSecs);
    TEST_ASSERT_EQUAL_UINT32(4, lc.at(1).tsSecs);
}
```

- [ ] **Step 3: Rodar → RED.**

- [ ] **Step 4: Implementar** — formato serializado: `magic(4) count(2) reservado(2)` + `count × registro de 16 B` (campos escritos individualmente em little-endian, mais antigo → mais novo) + `crc32(4)` sobre tudo antes do CRC. `deserialize` valida magic/tamanho/CRC, faz `clear()` e `append` na ordem lida (o próprio ring descarta os antigos se `cap` menor).

- [ ] **Step 5: Bump suite-count → 49 (mesmo commit). Rodar `-f test_audit_log` → GREEN. Commit** — `feat(irrigation): AuditLog ring serializável (§8.9)`

---

# MARCO 2 — Cola no `IrrigationModule`

> As Tasks 6–8 não têm teste nativo próprio (cola); a validação é a suíte inteira compilar + passar
> (`./bin/run-tests.sh -f test_irrigation_protocol` compila o firmware todo) e o CI ESP32 após push.

## Task 6: GPO fim-a-fim no módulo

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`
- Modify: `src/modules/irrigation/IrrigationModule.cpp`

- [ ] **Step 1: Driver + membro.** Em `IrrigationModule.h`, ao lado de `GpioValveDriver`:

```cpp
// Saída de nível (relé/MOSFET) dos GPOs.
class GpioGpoDriver : public IGpoDriver
{
  public:
    void configure(const IrrigationSettings &s);
    void set(uint8_t index, bool on) override;

  private:
    int8_t pins[IrrigationSettings::MAX_GPO] = {-1, -1};
};
```

Include `modules/irrigation/GpoController.h`; membros (após `valves`): `GpioGpoDriver gpoDriver; GpoController gpos;`. Declarar `void handleCmdGpo(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);` junto de `handleCmdValvula`. No `.cpp`, implementar o driver no molde do `GpioValveDriver::configure/pulse` existente (pinMode OUTPUT nos pinos != -1; `set` faz digitalWrite). Construtor: `gpos(gpoDriver, 0)` — contagem real vem de `activateSettings`/boot.

- [ ] **Step 2: Contagem de GPOs derivada dos pinos.** Helper no `.cpp`:

```cpp
static uint8_t countGpos(const IrrigationSettings &s)
{
    uint8_t n = 0;
    for (int i = 0; i < IrrigationSettings::MAX_GPO; i++)
        if (s.pinsGpo[i] >= 0)
            n++;
    return n;
}
```

No boot (onde `driver.configure(settings)` roda) e em `activateSettings`: `gpoDriver.configure(settings); gpos.setNumGpos(countGpos(settings));`.

- [ ] **Step 3: `handleCmdGpo`** — espelho exato de `handleCmdValvula` (auth → seq replay → rate-limit → decode `decodeCmdGpo` → `safeMode && action==1` ⇒ `REASON_SAFE_MODE`) despachando `gpos.command(cmd.gpoId, cmd.action, cmd.durationS, millis())`; `INVALID_ID` ⇒ `REASON_INVALID_ID`. Registrar no switch de `handleReceived` (`case MSG_CMD_GPO:` ao lado de `MSG_CMD_VALVULA`).

- [ ] **Step 4: Estado real.** `sendAck`: `ack.gpoStates = gpos.states();`. `sendHeartbeat`: `hb.gpoStates = gpos.states();`. `portalFillNodeState`: substituir a linha `out.gpoStates = 0; // sem acessor...` por `out.gpoStates = gpos.states();` (e `numGpos` se o ctx ganhar o campo na Task 9). `runOnce`: `gpos.tick(millis());` junto do `valves.tick`. Modo seguro: `grep -n "safeMode = true" src/modules/irrigation/IrrigationModule.cpp` e em cada ponto adicionar `gpos.allOff();`; `factoryReset`: `gpos.allOff();` junto do fechamento de válvulas.

- [ ] **Step 5: Rodar `-f test_irrigation_protocol` (compila o firmware) → GREEN. Commit** — `feat(irrigation): GPO fim-a-fim na estação (MSG_CMD_GPO + estado real)`

## Task 7: Sampler + tamper no módulo

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `.cpp`

- [ ] **Step 1: Leitor Arduino + membro.** `IrrigationModule.h`: include `SensorSampler.h`; membro `SensorSampler sampler;` + estado do tamper:

```cpp
    // Tamper (§8.12): debounce próprio, fora dos 4 slots de sensor.
    bool tamperActive = false;
    bool tamperRawLast = false;
    uint32_t tamperRawSinceMs = 0;
    bool tamperInit = false;
```

No `.cpp`, leitor concreto (arquivo do módulo, topo):

```cpp
namespace
{
struct ArduinoSensorReader : ISensorReader {
    uint16_t readAdc(int8_t pin) override { return (uint16_t)analogRead(pin); }
    bool readLevel(int8_t pin) override { return digitalRead(pin) != 0; }
};
ArduinoSensorReader sensorReader;
} // namespace
```

Boot + `activateSettings`: `sampler.configure(settings);` e `pinMode(INPUT...)` nos pinos de sensor/tamper != -1 (INPUT_PULLUP quando ativo-baixo).

- [ ] **Step 2: `sendHeartbeat` carrega sensores + tamper.** Após montar `hb`:

```cpp
    hb.flags |= tamperActive ? HB_FLAG_TAMPER : 0;
    IrrigationProto::SensorReading rs[IrrigationSettings::MAX_SENSORS];
    hb.sensorCount = (uint8_t)sampler.readings(rs);
    for (uint8_t i = 0; i < hb.sensorCount; i++)
        hb.sensors[i] = rs[i];
```

e, após o `sendToMesh` bem-sucedido: `sampler.noteReported(millis());`.

- [ ] **Step 3: `runOnce`:** `sampler.tick(millis(), sensorReader);` + tamper com debounce de 200 ms fixo (mesma técnica do sampler digital: raw → polaridade por `hwFlags & 1` → estável). Na transição para **ativo**: `tamperActive = true;` + auditoria (Task 8) + `if (!portal.apShouldBeUp()) sendEvento(IrrigationProto::EV_TAMPER, 1);` (janela de manutenção automática = portal aberto). Transição para inativo: `sendEvento(EV_TAMPER, 0)` sob a mesma condição. Depois: `if (sampler.earlyHeartbeatDue(millis())) sendHeartbeat();`.

- [ ] **Step 4: Rodar `-f test_sensor_sampler` (compila tudo) → GREEN. Commit** — `feat(irrigation): sampler + tamper na estação (HB antecipado, EV_TAMPER)`

## Task 8: Auditoria plugada + persistência

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `.cpp`

- [ ] **Step 1: Membro + helper.** `IrrigationModule.h`: include `AuditLog.h`; membros:

```cpp
    static constexpr size_t AUDIT_CAP = 100; // mini-log da estação (§8.9)
    AuditRecord auditStore[AUDIT_CAP];
    AuditLog audit{auditStore, AUDIT_CAP};
    bool auditDirty = false;
    uint32_t lastAuditSaveMs = 0;
    void auditEvent(AuditOrigin o, AuditAction a, uint8_t target, AuditResult res, uint32_t node = 0, uint32_t seq = 0);
    bool loadAuditLog();
    bool saveAuditLog();
    const AuditLog &auditLogRef() const { return audit; } // portal (Task 10)
```

`.cpp` — `auditEvent` preenche `tsSecs` pela mesma fonte de RTC de `computeLocalSecs` (tolerando 0 sem RTC), `append`, `auditDirty = true`. `loadAuditLog`/`saveAuditLog` no molde exato de `loadAllowlist`/`saveAllowlist` (linhas ~794–830; path `/prefs/irrigation_log.dat`, staging + rename, buffer `uint8_t tmp[8 + AUDIT_CAP * 16 + 4]`).

- [ ] **Step 2: Flush com debounce** no `runOnce`: `if (auditDirty && millis() - lastAuditSaveMs >= 60000) { saveAuditLog(); auditDirty = false; lastAuditSaveMs = millis(); }`. Boot: `loadAuditLog();` + `auditEvent(AuditOrigin::SISTEMA, AuditAction::REBOOT, rebootCause, AuditResult::OK);` (uma vez, junto do heartbeat de boot).

- [ ] **Step 3: Pontos de registro** (cada um 1 linha `auditEvent(...)`):
  - `handleCmdValvula`: após o `sendAck` final — origem `PAINEL`, ação `ABRIR`/`FECHAR`, target `valveId`, result `OK`/`NACK`, node `mp.from`, seq `h.seq`.
  - `handleCmdGpo`: idem com `GPO_ON`/`GPO_OFF`.
  - `onButtonEvent`: caminhos de abrir/fechar/pulso manual — origem `BOTAO_FISICO`.
  - `portalPulse` / `portalRunNetCommand`: origem `PORTAL_CAMPO` (net command loga o **envio** do relay; resultado fim-a-fim é assunto do gateway/6b).
  - Fechamento por timer fail-safe: no `runOnce`, capturar `valves.stateBitmap()`/`gpos.states()` antes e depois dos `tick()`; bits que caíram sem comando ⇒ `FAILSAFE_TIMER`/`FECHAR` (ou `GPO_OFF`) por índice.
  - `commitPairing` ⇒ `PAREAR`; transições de `safeMode` ⇒ `SAFE_MODE_IN/OUT`; tamper (Task 7 Step 3) ⇒ `TAMPER` (target = 1 abriu/0 fechou); adoção de config em `handleSetConfig` pós-commit ⇒ `CONFIG_EPOCH`.
  - `factoryReset`: **apagar** o log (`audit.clear(); FSCom.remove("/prefs/irrigation_log.dat");`) — estação volta zerada (decisão de design).

- [ ] **Step 4: Rodar `-f test_audit_log` (compila tudo) → GREEN. Commit** — `feat(irrigation): auditoria plugada + persistência LittleFS`

---

# MARCO 3 — Portal

## Task 9: `PortalApi` — builders/parsers novos

**Files:**
- Modify: `src/modules/irrigation/PortalApi.h`, `.cpp`
- Test: `test/test_portal_api/test_main.cpp` (suíte existente)

- [ ] **Step 1: Testes que falham** — 1 por função (moldes da própria suíte):
  - `buildSensors`: ctx com 2 itens → JSON `{"sensors":[{"id":0,"tipo":1,"unidade":"bar","valor":152},{"id":1,"tipo":0,"unidade":"","valor":100}]}`.
  - `buildPortalLog`: `AuditLog` com 2 registros → `{"log":[{"ts":20,"origem":4,"acao":0,"alvo":1,"res":0},...]}` (mais recente primeiro; enums numéricos — nomes ficam no JS).
  - `parseGpoReq`: `{"gpo":1,"action":1,"durationS":0,"confirm":true}` → struct; `confirm` ausente → `false` no campo (política fica no módulo); campos fora de faixa ⇒ `ParseResult` de erro (mesmo padrão de `parsePulse`).
  - `buildCoords`/`parseCoords`: inteiros `latE7`/`lonE7` (JS converte decimal ↔ ×1e7; `JsonReader` não parseia float).

- [ ] **Step 2: Rodar `-f test_portal_api` → RED.**

- [ ] **Step 3: Implementar** em `PortalApi.h`/`.cpp` (reusar `JsonWriter`/`JsonReader`; incluir `AuditLog.h`):

```cpp
// --- Sensores / GPO / coords / mini-log (Fase 6a) ---
struct PortalSensorItem {
    uint8_t id = 0;
    uint8_t tipo = 0;    // 0 digital, 1 analógico
    uint8_t unidade = 0; // enum settings v4
    int16_t valueCenti = 0;
};
struct PortalSensorsCtx {
    uint8_t count = 0;
    PortalSensorItem items[4];
};
size_t buildSensors(const PortalSensorsCtx &ctx, char *buf, size_t cap);

size_t buildPortalLog(const AuditLog &log, char *buf, size_t cap);

struct PortalGpoReq {
    uint8_t gpoId = 0;
    uint8_t action = 0;
    uint16_t durationS = 0;
    bool confirm = false;
};
ParseResult parseGpoReq(const char *json, size_t len, PortalGpoReq &out);

struct PortalCoords {
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
};
size_t buildCoords(const PortalCoords &c, char *buf, size_t cap);
ParseResult parseCoords(const char *json, size_t len, PortalCoords &out);
```

Tabela de unidade no `.cpp`: `static const char *UNIT_NAMES[] = {"", "bar", "%", "m", "C"};` (índice fora ⇒ `""`). `valor` no JSON = centi cru (JS divide por 100 quando analógico).

- [ ] **Step 4: Rodar → GREEN. Commit** — `feat(irrigation): PortalApi sensores/GPO/coords/mini-log`

## Task 10: Endpoints `/api/portal/*` novos

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`, `.cpp` (serviços)
- Modify: `src/modules/irrigation/IrrigationPortalEndpoints.cpp`

- [ ] **Step 1: Serviços no módulo** (declarar no bloco "Serviço do portal de campo" do header):

```cpp
    void portalFillSensors(IrrigationWeb::PortalSensorsCtx &out) const;
    bool portalGpo(const IrrigationWeb::PortalGpoReq &r); // biestável exige confirm
    void portalGetCoords(IrrigationWeb::PortalCoords &out) const;
    bool portalSetCoords(const IrrigationWeb::PortalCoords &c); // persiste settings
```

`.cpp`: `portalFillSensors` copia de `sampler.readings` + `settings.sensores[i].unidade`; `portalGpo` rejeita `action==1 && safeMode`, rejeita `durationS==0 && action==1 && !confirm`, despacha `gpos.command(...)` + `auditEvent(PORTAL_CAMPO, ...)`; `portalSetCoords` grava `latE7/lonE7` em `settings` + `saveIrrigationSettings(settings)` (sem mexer em `configEpoch` — coordenada é local, §8.8).

- [ ] **Step 2: Rotas** em `IrrigationPortalEndpoints.cpp`, no molde das existentes (mesmo guard, mesmos helpers de resposta): `GET /api/portal/sensors`, `GET /api/portal/log`, `POST /api/portal/gpo`, `GET /api/portal/coords`, `POST /api/portal/coords`.

- [ ] **Step 3: Rodar `-f test_portal_api` (compila tudo; a cola é CI-gated) → GREEN. Commit** — `feat(irrigation): endpoints portal sensores/GPO/coords/log`

## Task 11: Frontend — aba "Este nó"

**Files:**
- Modify: `data/irrigacao/portal/index.html`, `data/irrigacao/portal/app.js`

- [ ] **Step 1: index.html** — na aba Este nó, adicionar seções (mesmo estilo/classes dos cartões existentes): `#sensores` (lista), `#gpos` (botões liga/desliga), badge tamper junto dos flags existentes, `#coords` (form lat/lon decimais + salvar), `#minilog` (tabela ts/origem/ação/resultado).

- [ ] **Step 2: app.js** — seguindo os padrões do arquivo (fetch + render por polling existente):
  - `GET /api/portal/sensors` no mesmo ciclo de poll do estado; analógico renderiza `valor/100` + unidade; digital "ativo/inativo".
  - GPO: botão chama `POST /api/portal/gpo`; quando `durationS==0` e ligar → `confirm()` nativo do browser antes, enviando `confirm:true` (§8.11).
  - Tamper: badge visível quando flag `HB_FLAG_TAMPER` do `/api/portal/state`.
  - Coords: carregar de `GET /api/portal/coords` (`latE7/1e7`), salvar com `Math.round(v*1e7)`.
  - Mini-log: `GET /api/portal/log` sob demanda (botão "Atualizar"); mapear enums → rótulos pt-BR num objeto JS (`ORIGENS`, `ACOES`, `RESULTADOS` — mesmos índices dos enums C++ de `AuditLog.h`).

- [ ] **Step 3: Commit** — `feat(irrigation): portal Este nó — sensores, GPO, tamper, coords, mini-log`

## Task 12: Roadmap, verificação final

**Files:**
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md`

- [ ] **Step 1: Roadmap** — linha da Fase 5: `concluída (5a 2026-07-15, 5b 2026-07-17)`. Linha da Fase 6: anotar divisão `6a (estação — este plano, docs/superpowers/plans/2026-07-20-irrigacao-fase6a-estacao.md) / 6b (gateway — futuro)`.

- [ ] **Step 2: Suíte inteira** — `docker run ... mesh-test ./bin/run-tests.sh` (sem filtro) → exit 0 GREEN, contagem 49.

- [ ] **Step 3: Commit** — `docs(irrigation): roadmap fase 5 concluída, fase 6 dividida em 6a/6b`

---

## Self-Review (executado na escrita do plano)

- **Cobertura da spec 6a**: settings v4 (T1), bloco HB + EV_TAMPER (T2), sampler (T3/T7), GPO (T4/T6), auditoria (T5/T8), portal API/endpoints/UI (T9–T11), coords 5b-follow-up (T1/T9/T10/T11), roadmap (T12). Janela de manutenção via painel, nomes de sensor, motor de intertravamentos: 6b (fora do plano, por design).
- **Tipos consistentes**: `SensorReading{id,tipo,valueCenti}` (T2) usado em T3/T7/T9; `PortalGpoReq`/`PortalCoords` (T9) usados em T10; `AuditLog(AuditRecord*, size_t)` (T5) usado em T8/T9.
- **Placeholders**: nenhum "TBD"; T4 Step 4 e T6/T8 descrevem edits sobre código existente com âncoras de linha/grep + contratos completos nos headers.
