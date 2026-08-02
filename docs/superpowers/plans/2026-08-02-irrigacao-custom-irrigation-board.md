# custom_irrigation board — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `custom_irrigation` PlatformIO variant (electrically a Heltec WiFi LoRa 32 V2, no GPS) whose `variant.h` seeds the irrigation pin map (2 latch valves, 2 inputs, tamper, GPO, button, LED), applied as first-boot defaults, so a freshly flashed board runs on the bench without portal provisioning and is re-pinned for custom hardware by editing one file.

**Architecture:** Three additive pieces — (1) a header-only mapper `applyBoardIrrigationDefaults()` that fills `IrrigationSettings` pin fields from `IRRIGATION_PIN_*` variant macros (no-op when undefined); (2) a one-line hook in `loadIrrigationSettingsOrDefault()` that calls the mapper only when no blob is persisted (first boot / post-factory-reset); (3) the `custom_irrigation` variant folder that defines the macros. No ABI change (`IrrigationSettings` v6 / 180 B intact), no protocol change.

**Tech Stack:** C++ (ESP32 / Arduino), PlatformIO, Unity native tests, Docker native suite.

## Global Constraints

- ABI locked: `IrrigationSettings` v6 = 180 B. Do NOT touch the struct layout. Verbatim from spec: "Aditivo total: sem mudança de ABI nem de protocolo (VERSION=1)."
- Mapper mutates ONLY pin/count fields (`numValves`, `pinsHbridgeA/B`, `pinsDigitalIn`, `digitalInActiveLow`, `pinsGpo`, `pinBtn`, `pinLed`, `pinTamper`, `hwFlags`, `sensores[0]`). Never `role`, `boundGateway`, `configEpoch`, tables.
- Mapper is a no-op unless `IRRIGATION_PIN_VALVE0_A` is defined → native and all non-custom boards behave exactly as today.
- Heltec V2 seed pin map (no GPS): valves 17/23 + 22/32, inputs 38/39, tamper 37 (active-low), GPO0 33, button 0, LED 25, sensor0 36 (off by default). RTC = external DS3231 on I2C, NOT in the blob.
- Native suite baseline GREEN = all suites in `test/`; `test/native-suite-count` currently `64`, becomes `65`.
- Run native suite in Docker (Windows host): see `bin/test-native-docker.sh` / memory workaround `cp -a $(ls -A | grep -vx ".claude") /tmp/fw-test/`.
- ESP32 build of env `custom_irrigation` is CI/bancada-only (not built by the native suite).

---

## File Structure

- `src/modules/irrigation/IrrigationBoardDefaults.h` — NEW. Header-only `applyBoardIrrigationDefaults(IrrigationSettings&)`.
- `test/test_irrigation_board_defaults/test_main.cpp` — NEW. Native Unity suite for the mapper.
- `test/native-suite-count` — MODIFY. `64` → `65`.
- `src/modules/irrigation/IrrigationModule.cpp` — MODIFY. Include mapper + hook in `loadIrrigationSettingsOrDefault()`.
- `variants/esp32/custom_irrigation/platformio.ini` — NEW. Env `custom_irrigation`.
- `variants/esp32/custom_irrigation/variant.h` — NEW. Heltec V2 HW defines (no GPS) + `IRRIGATION_PIN_*` block.
- `docs/irrigacao/custom-irrigation-board.md` — NEW. Pin table, RTC wiring, build/flash, re-pin guide.

---

### Task 1: Mapper header + native test

**Files:**
- Create: `src/modules/irrigation/IrrigationBoardDefaults.h`
- Test: `test/test_irrigation_board_defaults/test_main.cpp`
- Modify: `test/native-suite-count`

**Interfaces:**
- Consumes: `IrrigationSettings` (from `IrrigationSettings.h`), `IrrigationRole`.
- Produces: `void applyBoardIrrigationDefaults(IrrigationSettings &s)` — fills pin fields from `IRRIGATION_PIN_*` macros; no-op if `IRRIGATION_PIN_VALVE0_A` undefined.

- [ ] **Step 1: Write the failing test**

Create `test/test_irrigation_board_defaults/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"

// Simula o variant custom_irrigation (seed Heltec V2, sem GPS): define as macros
// ANTES de incluir o header do mapper.
#define IRRIGATION_NUM_VALVES        2
#define IRRIGATION_PIN_VALVE0_A      17
#define IRRIGATION_PIN_VALVE0_B      23
#define IRRIGATION_PIN_VALVE1_A      22
#define IRRIGATION_PIN_VALVE1_B      32
#define IRRIGATION_PIN_DIN0          38
#define IRRIGATION_PIN_DIN1          39
#define IRRIGATION_DIN_ACTIVE_LOW    0x03
#define IRRIGATION_PIN_TAMPER        37
#define IRRIGATION_TAMPER_ACTIVE_LOW 1
#define IRRIGATION_PIN_GPO0          33
#define IRRIGATION_PIN_BTN           0
#define IRRIGATION_PIN_LED           25

#include "modules/irrigation/IrrigationBoardDefaults.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_heltec_map_pins()
{
    IrrigationSettings s; // defaults (pinos -1)
    applyBoardIrrigationDefaults(s);
    TEST_ASSERT_EQUAL_INT8(2, s.numValves);
    TEST_ASSERT_EQUAL_INT8(17, s.pinsHbridgeA[0]);
    TEST_ASSERT_EQUAL_INT8(23, s.pinsHbridgeB[0]);
    TEST_ASSERT_EQUAL_INT8(22, s.pinsHbridgeA[1]);
    TEST_ASSERT_EQUAL_INT8(32, s.pinsHbridgeB[1]);
    TEST_ASSERT_EQUAL_INT8(38, s.pinsDigitalIn[0]);
    TEST_ASSERT_EQUAL_INT8(39, s.pinsDigitalIn[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, s.digitalInActiveLow);
    TEST_ASSERT_EQUAL_INT8(37, s.pinTamper);
    TEST_ASSERT_EQUAL_UINT8(0x01, s.hwFlags & 0x01);
    TEST_ASSERT_EQUAL_INT8(33, s.pinsGpo[0]);
    TEST_ASSERT_EQUAL_INT8(0, s.pinBtn);
    TEST_ASSERT_EQUAL_INT8(25, s.pinLed);
}

static void test_sentinels_untouched()
{
    IrrigationSettings s;
    applyBoardIrrigationDefaults(s);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsHbridgeA[2]); // válvula não mapeada
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsHbridgeB[7]);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsGpo[1]);       // GPO1 não mapeado
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsDigitalIn[2]); // entrada não mapeada
}

static void test_non_pin_fields_preserved()
{
    IrrigationSettings s;
    s.role = (uint8_t)IrrigationRole::GATEWAY;
    s.boundGateway = 0xAABBCCDD;
    s.configEpoch = 42;
    applyBoardIrrigationDefaults(s);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)IrrigationRole::GATEWAY, s.role);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, s.boundGateway);
    TEST_ASSERT_EQUAL_UINT32(42, s.configEpoch);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_heltec_map_pins);
    RUN_TEST(test_sentinels_untouched);
    RUN_TEST(test_non_pin_fields_preserved);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 2: Run the test to verify it fails**

Run (single suite; on Windows host use the Docker native runner per memory `native-test-docker-cp-workaround`):
```bash
./bin/run-tests.sh -f test_irrigation_board_defaults
```
Expected: RED — build error, `IrrigationBoardDefaults.h` not found / `applyBoardIrrigationDefaults` undefined.

- [ ] **Step 3: Write minimal implementation**

Create `src/modules/irrigation/IrrigationBoardDefaults.h`:

```cpp
#pragma once
#include "IrrigationSettings.h"

// Aplica o mapa de pinos definido pelo variant da board (macros IRRIGATION_PIN_*).
// No-op se o build não definir nenhuma macro (nativo, boards não-irrigação) — nesses
// casos a config vem do portal/gateway como antes. Só mexe em campos de pino/contagem;
// nunca em role/boundGateway/configEpoch/tabelas.
static inline void applyBoardIrrigationDefaults(IrrigationSettings &s)
{
#if defined(IRRIGATION_PIN_VALVE0_A)
#ifdef IRRIGATION_NUM_VALVES
    s.numValves = IRRIGATION_NUM_VALVES;
#endif
    s.pinsHbridgeA[0] = IRRIGATION_PIN_VALVE0_A;
    s.pinsHbridgeB[0] = IRRIGATION_PIN_VALVE0_B;
#ifdef IRRIGATION_PIN_VALVE1_A
    s.pinsHbridgeA[1] = IRRIGATION_PIN_VALVE1_A;
    s.pinsHbridgeB[1] = IRRIGATION_PIN_VALVE1_B;
#endif
#ifdef IRRIGATION_PIN_DIN0
    s.pinsDigitalIn[0] = IRRIGATION_PIN_DIN0;
#endif
#ifdef IRRIGATION_PIN_DIN1
    s.pinsDigitalIn[1] = IRRIGATION_PIN_DIN1;
#endif
#ifdef IRRIGATION_DIN_ACTIVE_LOW
    s.digitalInActiveLow = IRRIGATION_DIN_ACTIVE_LOW;
#endif
#ifdef IRRIGATION_PIN_TAMPER
    s.pinTamper = IRRIGATION_PIN_TAMPER;
#if defined(IRRIGATION_TAMPER_ACTIVE_LOW) && IRRIGATION_TAMPER_ACTIVE_LOW
    s.hwFlags |= 0x01;
#endif
#endif
#ifdef IRRIGATION_PIN_GPO0
    s.pinsGpo[0] = IRRIGATION_PIN_GPO0;
#endif
#ifdef IRRIGATION_PIN_GPO1
    s.pinsGpo[1] = IRRIGATION_PIN_GPO1;
#endif
#ifdef IRRIGATION_PIN_BTN
    s.pinBtn = IRRIGATION_PIN_BTN;
#endif
#ifdef IRRIGATION_PIN_LED
    s.pinLed = IRRIGATION_PIN_LED;
#endif
#ifdef IRRIGATION_PIN_SENSOR0
    s.sensores[0].pino = IRRIGATION_PIN_SENSOR0;
    s.sensores[0].tipo = 1; // analógico
#endif
#else
    (void)s; // nenhuma macro de board → no-op
#endif
}
```

- [ ] **Step 4: Bump the suite count**

Edit `test/native-suite-count`: change `64` to `65`.

- [ ] **Step 5: Run the test to verify it passes**

Run: `./bin/run-tests.sh -f test_irrigation_board_defaults`
Expected: FILTERED, 3 test cases pass (test_heltec_map_pins, test_sentinels_untouched, test_non_pin_fields_preserved).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationBoardDefaults.h test/test_irrigation_board_defaults/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): board pin-map defaults mapper + native test"
```

---

### Task 2: Wire mapper into first-boot load

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (include ~line 14; function at lines 84-91)

**Interfaces:**
- Consumes: `applyBoardIrrigationDefaults()` (Task 1), `loadIrrigationSettings()` (returns `false` when no blob persisted).
- Produces: nothing new; `loadIrrigationSettingsOrDefault()` now applies board defaults on first boot.

- [ ] **Step 1: Add the include**

In `src/modules/irrigation/IrrigationModule.cpp`, after `#include "gps/RTC.h"` (line 14), add:

```cpp
#include "modules/irrigation/IrrigationBoardDefaults.h"
```

(Must be AFTER `#include "configuration.h"` on line 13 so the variant's `IRRIGATION_PIN_*` macros are in scope.)

- [ ] **Step 2: Add the hook**

Replace the body of `loadIrrigationSettingsOrDefault()` (lines 84-91):

```cpp
static IrrigationSettings loadIrrigationSettingsOrDefault()
{
    IrrigationSettings s;
    if (!loadIrrigationSettings(s))      // sem blob persistido = 1º boot / pós-factory-reset
        applyBoardIrrigationDefaults(s); // aplica o mapa de pinos da board (no-op sem variant custom)
    if (s.pulseMs > 1000)
        s.pulseMs = 1000;
    return s;
}
```

- [ ] **Step 3: Verify native module still builds + full suite GREEN**

Native has no `IRRIGATION_PIN_*` macros, so the mapper compiles to a no-op and `loadIrrigationSettings` returns `false` (no FSCom) — behavior unchanged. Run the FULL native suite in Docker:
```bash
./bin/run-tests.sh
```
Expected: `RESULT: GREEN 65/65 suites passed`.

- [ ] **Step 4: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): apply board pin-map defaults on first boot"
```

---

### Task 3: `custom_irrigation` variant + docs

**Files:**
- Create: `variants/esp32/custom_irrigation/platformio.ini`
- Create: `variants/esp32/custom_irrigation/variant.h`
- Create: `docs/irrigacao/custom-irrigation-board.md`

**Interfaces:**
- Produces: build env `custom_irrigation`; variant.h defines the `IRRIGATION_PIN_*` macros consumed by Task 1's mapper.

- [ ] **Step 1: Create the PlatformIO env**

Create `variants/esp32/custom_irrigation/platformio.ini`:

```ini
[env:custom_irrigation]
custom_meshtastic_hw_model = 255
custom_meshtastic_hw_model_slug = PRIVATE_HW
custom_meshtastic_architecture = esp32
custom_meshtastic_actively_supported = false
custom_meshtastic_display_name = Custom Irrigation (Heltec V2 seed)
custom_meshtastic_tags = Irrigation

board_level = extra
extends = esp32_base
board = heltec_wifi_lora_32_V2
build_flags =
  ${esp32_base.build_flags}
  -D HELTEC_V2_0
  -D CUSTOM_IRRIGATION
  -I variants/esp32/custom_irrigation
```

- [ ] **Step 2: Create the variant header**

Create `variants/esp32/custom_irrigation/variant.h`:

```c
// custom_irrigation — placa própria de irrigação.
// SEED elétrico = Heltec WiFi LoRa 32 V2, SEM GPS (GPIO33/36 livres p/ irrigação).
// Para a board real: trocar `board` no platformio.ini, os defines de HARDWARE abaixo
// (LoRa/OLED/ADC conforme a board) e o bloco IRRIGATION_* pelos pinos do L298N/relé.

// ================= HARDWARE (Heltec V2, sem GPS) =================
#define HAS_GPS 0 // sem GPS → GPIO33 e GPIO36 ficam livres para irrigação

#ifndef USE_JTAG // gpio15 é TDO do JTAG
#define I2C_SDA 4
#define I2C_SCL 15
#endif

#define RESET_OLED 16
#define VEXT_ENABLE 21 // active low: alimenta OLED e boost da antena LoRa
#define LED_POWER 25   // LED onboard (partilhado com pinLed da irrigação)
#define BUTTON_PIN 0   // botão PRG onboard (partilhado com pinBtn da irrigação)

#define USE_RF95
#define LORA_DIO0 26
#ifndef USE_JTAG
#define LORA_RESET 14
#endif
#define LORA_DIO1 35
#define LORA_DIO2 34

#define ADC_MULTIPLIER 3.2 // divisor R12=100k / R10=220k
#define BATTERY_PIN 13
#define ADC_CHANNEL ADC2_GPIO13_CHANNEL
#define BAT_MEASURE_ADC_UNIT 2

// ================= MAPA DE IRRIGAÇÃO (seed Heltec V2) =================
// Consumido por src/modules/irrigation/IrrigationBoardDefaults.h no 1º boot.
#define IRRIGATION_NUM_VALVES        2
#define IRRIGATION_PIN_VALVE0_A      17 // válvula 0 — abre (H-bridge A)
#define IRRIGATION_PIN_VALVE0_B      23 // válvula 0 — fecha (H-bridge B)
#define IRRIGATION_PIN_VALVE1_A      22 // válvula 1 — abre
#define IRRIGATION_PIN_VALVE1_B      32 // válvula 1 — fecha
#define IRRIGATION_PIN_DIN0          38 // entrada digital 0 (input-only, pull-up externo)
#define IRRIGATION_PIN_DIN1          39 // entrada digital 1 (input-only, pull-up externo)
#define IRRIGATION_DIN_ACTIVE_LOW    0x03 // entradas 0 e 1 ativo-baixo (pull-up externo)
#define IRRIGATION_PIN_TAMPER        37 // tamper (input-only)
#define IRRIGATION_TAMPER_ACTIVE_LOW 1
#define IRRIGATION_PIN_GPO0          33 // saída de nível (relé/bomba) — livre por não ter GPS
#define IRRIGATION_PIN_BTN           0  // botão multifunção (PRG onboard)
#define IRRIGATION_PIN_LED           25 // LED de estado (onboard)
// Opcional — sensor analógico 0 no ADC1 (GPIO36, livre sem GPS). Descomentar p/ ativar:
// #define IRRIGATION_PIN_SENSOR0    36
// GPO1 e válvulas 2..7: sem pino seguro livre na Heltec V2 — ativar na board real.
```

- [ ] **Step 3: Create the board doc**

Create `docs/irrigacao/custom-irrigation-board.md`:

```markdown
# Board `custom_irrigation`

Placa-alvo própria do sistema de irrigação. Hoje é **eletricamente uma Heltec WiFi
LoRa 32 V2, sem GPS** (bancada). Evolui para o hardware custom editando um ficheiro:
`variants/esp32/custom_irrigation/variant.h`.

## Build / flash

```
pio run -e custom_irrigation            # compila
pio run -e custom_irrigation -t upload  # grava na Heltec de bancada
```

O env stock `heltec-v2_0` fica intacto.

## Mapa de pinos — seed Heltec V2 (sem GPS)

| Função | GPIO | Nota |
|---|---|---|
| Válvula 0 abre / fecha | 17 / 23 | H-bridge (latch) |
| Válvula 1 abre / fecha | 22 / 32 | H-bridge (latch) |
| Entrada digital 0 / 1 | 38 / 39 | input-only, **pull-up externo**, ativo-baixo |
| Tamper | 37 | input-only, ativo-baixo |
| GPO 0 (relé/bomba) | 33 | livre por não ter GPS |
| Botão multifunção | 0 | PRG onboard (partilhado c/ botão do sistema) |
| LED de estado | 25 | onboard (partilhado c/ LED do sistema) |
| Sensor 0 (opcional) | 36 | ADC1, off por default |

O mapa é aplicado como **default de 1º boot** (sem config persistida). Depois de
provisionar (wizard/portal), o blob salvo tem prioridade e o mapa da board é ignorado.

## RTC

A board não tem RTC. Para agenda semanal com hora real, ligar um **DS3231 externo** ao
barramento I2C (SDA=GPIO4, SCL=GPIO15, partilhado com o OLED) — o módulo RTC do
Meshtastic auto-deteta. Sem RTC o sistema continua operacional (mirror/manual; scheduler
ocioso sem hora), conforme spec §5.

## Caveats de bancada (Heltec V2)

- Botão (GPIO0) e LED (GPIO25) são partilhados com o botão/LED do sistema Meshtastic;
  gestos de irrigação e ações do sistema coexistem.
- Entradas 38/39/37 são input-only → **exigem pull-up externo**.
- Só cabem 2 válvulas latch; GPO1 e sensores extra ficam para a board real.

## Re-pinar para a board real (L298N + relé FINDER)

1. Editar `variant.h`: trocar `board` (platformio.ini), os defines de HARDWARE e o bloco
   `IRRIGATION_*` pelos pinos do L298N (válvula0 = IN1/IN2, válvula1 = IN3/IN4) e do relé.
2. `pio run -e custom_irrigation -t upload`.
3. Provisionar papel no wizard de 1º boot.
```

- [ ] **Step 4: Sanity-check the native suite is unaffected**

The new variant is a separate env, not built by the native suite. Confirm nothing regressed:
```bash
./bin/run-tests.sh
```
Expected: `RESULT: GREEN 65/65 suites passed`.

- [ ] **Step 5: Commit**

```bash
git add variants/esp32/custom_irrigation/ docs/irrigacao/custom-irrigation-board.md
git commit -m "feat(irrigation): custom_irrigation variant (Heltec V2 seed) + board doc"
```

- [ ] **Step 6: ESP32 build (CI/bancada)**

The `custom_irrigation` env only builds under ESP32 toolchain (not native). Trigger on CI or bench:
```bash
pio run -e custom_irrigation
```
Expected: SUCCESS, links `IrrigationModule.cpp` with the variant macros in scope. If the local Windows host lacks the ESP32 toolchain, defer to fork CI and note it in the closeout.

---

## Notes for the executor

- Do NOT run `trunk fmt` on the Windows host (per project memory); follow the repo clang-format style manually; fork CI validates.
- Radio behavior (pairing, config over radio) is not native-testable — bench 2+ nodes required before field use, but that is out of scope for this plan (no wire/protocol change here).
- If `bin/run-tests.sh` can't run directly on Windows, use the Docker native runner with the `.claude`-exclusion workaround from project memory.
