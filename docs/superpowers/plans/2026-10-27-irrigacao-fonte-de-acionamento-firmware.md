# Fonte de Acionamento — Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unificar Modo Remoto + Modo Espelho numa única "Fonte de Acionamento" com política de entrada configurável (nível/borda × toggle/abre/fecha/segue), avaliada no nó-fonte, com feedback no LED de status de placa (sem pino dedicado).

**Architecture:** Uma classe pura `InputActuationPolicy` (uma instância por entrada digital) substitui `RemoteButtonEdge` e `MirrorMode`, emitindo intenção lógica `OPEN/CLOSE/TOGGLE`. `gwFireRemote` vira despachante único que aceita ação explícita. `RemoteTrigger` ganha campo `action`. Feedback de comando vira padrões transientes no `LedPatternController` existente. Greenfield: `IrrigationSettings` redefinida limpa (sem cadeia de migração v1→v8).

**Tech Stack:** C++ (PlatformIO/Arduino), Unity test framework rodando no ambiente nativo (portduino), `./bin/run-tests.sh`.

## Global Constraints

- Spec fonte da verdade: `docs/superpowers/specs/2026-10-27-irrigacao-fonte-de-acionamento-unificada-design.md`.
- Escopo: SÓ entrada digital (Remoto + Espelho). Controle de Nível e Interlock ficam de fora.
- Testes nativos: cada `test_*/test_main.cpp` usa `void setup()`/`void loop()` (portduino é dono do `main`), NÃO `int main()`. `setup()` chama `initializeTestEnvironment()`, `UNITY_BEGIN()`, `RUN_TEST(...)`, `exit(UNITY_END())`.
- Rodar testes: `./bin/run-tests.sh` (exit 0 GREEN · 1 RED · 2 AMBER · 3 FILTERED). Ambiente nativo via Docker no Windows (`MSYS_NO_PATHCONV=1`).
- Formato: `trunk fmt` (no Windows, reproduzir clang-format@16.0.3 nas linhas alteradas).
- Commits frequentes, um por task no mínimo. Mensagens em pt-BR, estilo `feat(irrigation):`/`refactor(irrigation):`.
- Push de feature → remote `fork`, não `origin`.
- Polaridade é aplicada DENTRO da política (campo `activeLow`), não a montante.

---

### Task 1: `InputActuationPolicy` — avaliador puro unificado

**Files:**
- Create: `src/modules/irrigation/InputActuationPolicy.h`
- Create: `src/modules/irrigation/InputActuationPolicy.cpp`
- Create: `test/test_input_actuation_policy/test_main.cpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class InputMode : uint8_t { DISABLED = 0, FOLLOW, EDGE_BOTH, EDGE_TOGGLE, EDGE_OPEN, EDGE_CLOSE };

  class InputActuationPolicy {
    public:
      enum class Intent : uint8_t { NONE, OPEN, CLOSE, TOGGLE };
      static constexpr uint32_t DEBOUNCE_EDGE_MS = 50;
      static constexpr uint32_t DEBOUNCE_LEVEL_MS = 100;
      static constexpr uint32_t RENEW_MS = 60000;
      void configure(InputMode mode, bool activeLow); // reset de estado ao reconfigurar
      Intent update(bool rawHigh, uint32_t nowMs);     // rawHigh = GPIO HIGH; polaridade aplicada aqui
      bool active() const;                             // pós-debounce (p/ FOLLOW: bypass do scheduler)
      InputMode mode() const;
  };
  ```
- Semântica: polaridade — `pressed = activeLow ? !rawHigh : rawHigh`. `DISABLED`→sempre `NONE`, `active()`=false. `FOLLOW`: debounce 100 ms; na borda de subida (de inativo→ativo) emite `OPEN`, na de descida emite `CLOSE`; enquanto ativo, re-emite `OPEN` a cada `RENEW_MS`. `EDGE_BOTH`: debounce 50 ms; subida→`OPEN`, descida→`CLOSE`; sem renovação. `EDGE_TOGGLE`: debounce 50 ms; subida→`TOGGLE`; rearma só após soltar. `EDGE_OPEN`/`EDGE_CLOSE`: subida→`OPEN`/`CLOSE`; rearma após soltar.

- [ ] **Step 1: Escrever o teste que falha**

`test/test_input_actuation_policy/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/InputActuationPolicy.h"
#include <unity.h>

using Intent = InputActuationPolicy::Intent;

void setUp(void) {}
void tearDown(void) {}

// estabiliza um nível por >= debounceMs chamando a cada 25 ms; retorna a última intenção != NONE
static Intent settle(InputActuationPolicy &p, bool rawHigh, uint32_t from, uint32_t spanMs = 125)
{
    Intent last = Intent::NONE;
    for (uint32_t t = from; t <= from + spanMs; t += 25) {
        Intent a = p.update(rawHigh, t);
        if (a != Intent::NONE)
            last = a;
    }
    return last;
}

static void test_disabled_never_fires()
{
    InputActuationPolicy p;
    p.configure(InputMode::DISABLED, false);
    TEST_ASSERT_EQUAL(Intent::NONE, settle(p, true, 1000));
    TEST_ASSERT_FALSE(p.active());
}

static void test_follow_rise_open_fall_close()
{
    InputActuationPolicy p;
    p.configure(InputMode::FOLLOW, false);
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(p, true, 1000));
    TEST_ASSERT_TRUE(p.active());
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(p, false, 5000));
    TEST_ASSERT_FALSE(p.active());
}

static void test_follow_renews_every_60s()
{
    InputActuationPolicy p;
    p.configure(InputMode::FOLLOW, false);
    settle(p, true, 1000);
    TEST_ASSERT_EQUAL(Intent::NONE, p.update(true, 30000));
    TEST_ASSERT_EQUAL(Intent::OPEN, p.update(true, 1000 + 125 + 60000));
}

static void test_follow_active_low_inverts()
{
    InputActuationPolicy p;
    p.configure(InputMode::FOLLOW, true); // ativo-baixo: LOW = ativo
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(p, false, 1000));
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(p, true, 5000));
}

static void test_edge_both_rise_open_fall_close_no_renew()
{
    InputActuationPolicy p;
    p.configure(InputMode::EDGE_BOTH, false);
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(p, true, 1000, 75));
    TEST_ASSERT_EQUAL(Intent::NONE, p.update(true, 1000 + 75 + 60000)); // sem renovação
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(p, false, 200000, 75));
}

static void test_edge_toggle_once_per_press()
{
    InputActuationPolicy p;
    p.configure(InputMode::EDGE_TOGGLE, false);
    TEST_ASSERT_EQUAL(Intent::TOGGLE, settle(p, true, 1000, 75));
    TEST_ASSERT_EQUAL(Intent::NONE, settle(p, true, 2000, 75)); // segurar não re-dispara
    settle(p, false, 3000, 75);                                 // solta (rearma)
    TEST_ASSERT_EQUAL(Intent::TOGGLE, settle(p, true, 4000, 75));
}

static void test_edge_open_and_close_fixed()
{
    InputActuationPolicy po;
    po.configure(InputMode::EDGE_OPEN, false);
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(po, true, 1000, 75));
    InputActuationPolicy pc;
    pc.configure(InputMode::EDGE_CLOSE, false);
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(pc, true, 1000, 75));
}

static void test_glitch_shorter_than_debounce_ignored()
{
    InputActuationPolicy p;
    p.configure(InputMode::FOLLOW, false);
    p.update(true, 1000);
    p.update(false, 1050); // caiu antes de 100 ms
    TEST_ASSERT_EQUAL(Intent::NONE, p.update(false, 1200));
    TEST_ASSERT_FALSE(p.active());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_disabled_never_fires);
    RUN_TEST(test_follow_rise_open_fall_close);
    RUN_TEST(test_follow_renews_every_60s);
    RUN_TEST(test_follow_active_low_inverts);
    RUN_TEST(test_edge_both_rise_open_fall_close_no_renew);
    RUN_TEST(test_edge_toggle_once_per_press);
    RUN_TEST(test_edge_open_and_close_fixed);
    RUN_TEST(test_glitch_shorter_than_debounce_ignored);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 2: Rodar e confirmar que falha (compilação/link)**

Run: `./bin/run-tests.sh test_input_actuation_policy`
Expected: FALHA — `InputActuationPolicy.h` não existe.

- [ ] **Step 3: Implementar o cabeçalho e a classe**

`src/modules/irrigation/InputActuationPolicy.h`:
```cpp
#pragma once
#include <stdint.h>

enum class InputMode : uint8_t { DISABLED = 0, FOLLOW, EDGE_BOTH, EDGE_TOGGLE, EDGE_OPEN, EDGE_CLOSE };

// Avaliador puro de UMA entrada digital. Aplica polaridade, debounce (dependente
// do modo), detecção de nível/borda e renovação de 60 s (FOLLOW). Substitui
// RemoteButtonEdge + MirrorMode. Testável sem GPIO.
class InputActuationPolicy {
  public:
    enum class Intent : uint8_t { NONE, OPEN, CLOSE, TOGGLE };
    static constexpr uint32_t DEBOUNCE_EDGE_MS = 50;
    static constexpr uint32_t DEBOUNCE_LEVEL_MS = 100;
    static constexpr uint32_t RENEW_MS = 60000;

    void configure(InputMode mode, bool activeLow);
    Intent update(bool rawHigh, uint32_t nowMs);
    bool active() const { return _stable; }
    InputMode mode() const { return _mode; }

  private:
    uint32_t debounceMs() const { return _mode == InputMode::FOLLOW ? DEBOUNCE_LEVEL_MS : DEBOUNCE_EDGE_MS; }
    InputMode _mode = InputMode::DISABLED;
    bool _activeLow = false;
    bool _lastRaw = false;   // último "pressed" visto (pós-polaridade)
    uint32_t _sinceMs = 0;   // quando o raw mudou
    bool _stable = false;    // pós-debounce
    bool _armed = true;      // p/ EDGE_TOGGLE/OPEN/CLOSE: rearma ao soltar
    uint32_t _lastRenewMs = 0;
};
```

`src/modules/irrigation/InputActuationPolicy.cpp`:
```cpp
#include "modules/irrigation/InputActuationPolicy.h"

void InputActuationPolicy::configure(InputMode mode, bool activeLow)
{
    _mode = mode;
    _activeLow = activeLow;
    _lastRaw = false;
    _sinceMs = 0;
    _stable = false;
    _armed = true;
    _lastRenewMs = 0;
}

InputActuationPolicy::Intent InputActuationPolicy::update(bool rawHigh, uint32_t nowMs)
{
    if (_mode == InputMode::DISABLED)
        return Intent::NONE;

    const bool pressed = _activeLow ? !rawHigh : rawHigh;

    if (pressed != _lastRaw) {
        _lastRaw = pressed;
        _sinceMs = nowMs;
    }

    if ((uint32_t)(nowMs - _sinceMs) >= debounceMs() && pressed != _stable) {
        _stable = pressed;
        if (_stable) { // borda de subida estável
            _lastRenewMs = nowMs;
            switch (_mode) {
            case InputMode::FOLLOW:
            case InputMode::EDGE_BOTH:
            case InputMode::EDGE_OPEN:
                if (_mode != InputMode::FOLLOW && _mode != InputMode::EDGE_BOTH && !_armed)
                    break;
                _armed = false;
                return Intent::OPEN;
            case InputMode::EDGE_CLOSE:
                if (!_armed)
                    break;
                _armed = false;
                return Intent::CLOSE;
            case InputMode::EDGE_TOGGLE:
                if (!_armed)
                    break;
                _armed = false;
                return Intent::TOGGLE;
            default:
                break;
            }
        } else { // borda de descida estável
            _armed = true; // rearma modos de borda-subida
            if (_mode == InputMode::FOLLOW || _mode == InputMode::EDGE_BOTH)
                return Intent::CLOSE;
        }
    }

    // Renovação só no FOLLOW enquanto ativo
    if (_mode == InputMode::FOLLOW && _stable && (uint32_t)(nowMs - _lastRenewMs) >= RENEW_MS) {
        _lastRenewMs = nowMs;
        return Intent::OPEN;
    }
    return Intent::NONE;
}
```

Nota de implementação: a ramificação `OPEN` acima trata `FOLLOW`/`EDGE_BOTH` (sempre, sem `_armed`) e `EDGE_OPEN` (respeita `_armed`). Se preferir clareza, separar em `case` distintos — contanto que os testes do Step 1 passem.

- [ ] **Step 4: Rodar e confirmar verde**

Run: `./bin/run-tests.sh test_input_actuation_policy`
Expected: PASS (8 testes).

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/InputActuationPolicy.h src/modules/irrigation/InputActuationPolicy.cpp test/test_input_actuation_policy/test_main.cpp
git add src/modules/irrigation/InputActuationPolicy.h src/modules/irrigation/InputActuationPolicy.cpp test/test_input_actuation_policy/test_main.cpp
git commit -m "feat(irrigation): InputActuationPolicy (avaliador unificado de entrada)"
```

---

### Task 2: `RemoteTrigger` ganha `action`

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h` (struct `RemoteTrigger`)
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp:355-368` (`encodeRemoteTrigger`/`decodeRemoteTrigger`)
- Modify: `test/test_irrigation_protocol/test_main.cpp` (acrescenta caso)

**Interfaces:**
- Consumes: Task 1 `InputActuationPolicy::Intent` (conceito de ação).
- Produces:
  ```cpp
  struct RemoteTrigger { uint8_t inputIdx = 0; uint8_t action = 0; }; // 0=TOGGLE,1=OPEN,2=CLOSE
  ```
  Mapeamento `Intent → action`: `TOGGLE=0, OPEN=1, CLOSE=2` (constantes `ACTION_TOGGLE/OPEN/CLOSE` — reusar se já existirem em IrrigationProtocol; senão definir).

- [ ] **Step 1: Escrever o teste que falha**

Acrescentar em `test/test_irrigation_protocol/test_main.cpp` (e no `RUN_TEST` do `setup()`):
```cpp
static void test_remote_trigger_carries_action()
{
    using namespace IrrigationProto;
    uint8_t buf[32];
    RemoteTrigger in{2 /*inputIdx*/, 1 /*OPEN*/};
    size_t n = encodeRemoteTrigger(buf, sizeof(buf), 7, in);
    TEST_ASSERT_GREATER_THAN(0, n);
    RemoteTrigger out;
    TEST_ASSERT_TRUE(decodeRemoteTrigger(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(2, out.inputIdx);
    TEST_ASSERT_EQUAL_UINT8(1, out.action);
}
```

- [ ] **Step 2: Rodar e confirmar que falha**

Run: `./bin/run-tests.sh test_irrigation_protocol`
Expected: FALHA — `RemoteTrigger` não tem `action`.

- [ ] **Step 3: Implementar**

Em `IrrigationProtocol.h`, struct `RemoteTrigger`:
```cpp
struct RemoteTrigger {
    uint8_t inputIdx = 0;
    uint8_t action = 0; // ACTION_TOGGLE=0, ACTION_OPEN=1, ACTION_CLOSE=2
};
```
Em `IrrigationProtocol.cpp`:
```cpp
size_t encodeRemoteTrigger(uint8_t *buf, size_t len, uint32_t seq, const RemoteTrigger &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_REMOTE_TRIGGER, seq);
    w.u8(m.inputIdx);
    w.u8(m.action);
    return w.ok ? w.pos : 0;
}

bool decodeRemoteTrigger(const uint8_t *buf, size_t len, RemoteTrigger &out)
{
    Reader r = bodyReader(buf, len);
    out.inputIdx = r.u8();
    out.action = r.u8();
    return r.ok;
}
```

- [ ] **Step 4: Rodar e confirmar verde**

Run: `./bin/run-tests.sh test_irrigation_protocol`
Expected: PASS.

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationProtocol.cpp test/test_irrigation_protocol/test_main.cpp
git add src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationProtocol.cpp test/test_irrigation_protocol/test_main.cpp
git commit -m "feat(irrigation): RemoteTrigger carrega action (toggle/open/close)"
```

---

### Task 3: `LedPatternController` — padrões transientes de comando

**Files:**
- Modify: `src/modules/irrigation/LedPattern.h`
- Modify: `src/modules/irrigation/LedPattern.cpp` (`ledOn`)
- Test: `test/test_irrigation_ui/test_main.cpp` (se já cobre LedPattern) OU criar `test/test_led_pattern/test_main.cpp`

**Interfaces:**
- Produces: `enum class Mode` ganha `CMD_INFLIGHT, CMD_OK, CMD_FAIL`. `ledOn(nowMs)` rende os padrões: `CMD_INFLIGHT` = pisca rápido (período 150 ms), `CMD_OK` = 2 pulsos curtos, `CMD_FAIL` = rajada SOS curta. A JANELA transiente e a prioridade moram no chamador (Task 6), não aqui — aqui só o desenho do padrão por modo.

- [ ] **Step 1: Escrever o teste que falha**

Criar `test/test_led_pattern/test_main.cpp`:
```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/LedPattern.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_cmd_inflight_blinks_fast()
{
    LedPatternController c;
    c.setMode(LedPatternController::Mode::CMD_INFLIGHT);
    // pisca rápido: estados diferentes dentro de 300 ms
    bool a = c.ledOn(0);
    bool b = c.ledOn(160);
    TEST_ASSERT_NOT_EQUAL(a, b);
}

static void test_cmd_ok_is_mostly_on_then_known_pattern()
{
    LedPatternController c;
    c.setMode(LedPatternController::Mode::CMD_OK);
    // padrão determinístico — só garante que ledOn é função do tempo e não trava
    (void)c.ledOn(0);
    (void)c.ledOn(500);
    TEST_ASSERT_EQUAL(LedPatternController::Mode::CMD_OK, c.currentMode());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_cmd_inflight_blinks_fast);
    RUN_TEST(test_cmd_ok_is_mostly_on_then_known_pattern);
    exit(UNITY_END());
}
void loop() {}
```

- [ ] **Step 2: Rodar e confirmar que falha**

Run: `./bin/run-tests.sh test_led_pattern`
Expected: FALHA — modos `CMD_*` não existem.

- [ ] **Step 3: Implementar**

Em `LedPattern.h`, estender o enum:
```cpp
enum class Mode : uint8_t {
    NORMAL, NO_GATEWAY, CONFIG_PENDING, PAIRING, OUTPUT_OPEN, BATTERY_SOS,
    CMD_INFLIGHT, CMD_OK, CMD_FAIL
};
```
Em `LedPattern.cpp`, no `ledOn`, acrescentar os casos (seguir o estilo da tabela existente). Exemplo de padrões:
```cpp
case Mode::CMD_INFLIGHT: return ((nowMs / 150u) & 1u) != 0;              // pisca rápido
case Mode::CMD_OK:       { uint32_t t = nowMs % 1000u; return t < 120u || (t >= 240u && t < 360u); } // 2 pulsos
case Mode::CMD_FAIL:     { uint32_t t = nowMs % 1200u; return t < 100u || (t >= 200u && t < 300u) || (t >= 400u && t < 500u); } // rajada
```

- [ ] **Step 4: Rodar e confirmar verde**

Run: `./bin/run-tests.sh test_led_pattern`
Expected: PASS.

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/LedPattern.h src/modules/irrigation/LedPattern.cpp test/test_led_pattern/test_main.cpp
git add src/modules/irrigation/LedPattern.h src/modules/irrigation/LedPattern.cpp test/test_led_pattern/test_main.cpp
git commit -m "feat(irrigation): padroes CMD_INFLIGHT/CMD_OK/CMD_FAIL no LED de status"
```

---

### Task 4: `IrrigationSettings` — `digitalInMode[]`, remove campos de LED dedicado

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h`
- Modify: `src/modules/irrigation/IrrigationSettings.cpp` (defaults/load/save; remover migração v1→v8 legada)
- Test: `test/test_irrigation_config/test_main.cpp`

**Interfaces:**
- Consumes: Task 1 `InputMode`.
- Produces: em `IrrigationSettings`:
  ```cpp
  InputMode digitalInMode[MAX_DIGITAL_IN] = { InputMode::DISABLED, InputMode::DISABLED, InputMode::DISABLED, InputMode::DISABLED };
  // digitalInActiveLow permanece (polaridade).
  // REMOVIDOS: pinsRemoteLed[2], digitalInLedIdx, digitalInBtnMask (substituído por digitalInMode != DISABLED).
  ```
  Greenfield: `version = 1` do esquema novo; `magic` novo (ex.: `0x49525232` "IRR2") para que blobs antigos sejam rejeitados e caiam no default. `static_assert(sizeof(IrrigationSettings) == ...)` recalculado.

**Nota:** `digitalInBtnMask`/`digitalInLedIdx` eram packed; a helper `digitalInIsButton(s,i)` passa a ser `s.digitalInMode[i] != InputMode::DISABLED`. A helper `digitalInLedSlot` é removida (sem LED dedicado).

- [ ] **Step 1: Escrever o teste que falha**

Acrescentar em `test/test_irrigation_config/test_main.cpp`:
```cpp
static void test_digitalInMode_defaults_disabled_and_roundtrips()
{
    IrrigationSettings s;
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++)
        TEST_ASSERT_EQUAL(InputMode::DISABLED, s.digitalInMode[i]);
    s.digitalInMode[1] = InputMode::FOLLOW;
    s.digitalInMode[2] = InputMode::EDGE_BOTH;
    s.digitalInActiveLow = 0b0010;
    uint8_t buf[sizeof(IrrigationSettings) + 16];
    // usar o mesmo mecanismo de serialização que o load/save usa (cópia do blob)
    memcpy(buf, &s, sizeof(s));
    IrrigationSettings r;
    memcpy(&r, buf, sizeof(r));
    TEST_ASSERT_EQUAL(InputMode::FOLLOW, r.digitalInMode[1]);
    TEST_ASSERT_EQUAL(InputMode::EDGE_BOTH, r.digitalInMode[2]);
    TEST_ASSERT_EQUAL_UINT8(0b0010, r.digitalInActiveLow);
}
```
(Ajustar ao mecanismo real de (de)serialização em `IrrigationSettings.cpp` — se há `migrateIrrigationSettings`, testar via ele.)

- [ ] **Step 2: Rodar e confirmar que falha**

Run: `./bin/run-tests.sh test_irrigation_config`
Expected: FALHA — `digitalInMode` não existe.

- [ ] **Step 3: Implementar**

- Adicionar `#include "modules/irrigation/InputActuationPolicy.h"` em `IrrigationSettings.h`.
- Adicionar `InputMode digitalInMode[MAX_DIGITAL_IN]`.
- Remover `pinsRemoteLed[2]`, `digitalInLedIdx`, `digitalInBtnMask` e os `static_assert`/offsets relativos a eles.
- Trocar `magic`/`version` para o esquema novo; **remover** a cadeia de migração v1..v8 em `IrrigationSettings.cpp` (greenfield: blob com magic antigo → default).
- Atualizar helper em `IrrigationSettings.h`:
  ```cpp
  inline bool digitalInIsButton(const IrrigationSettings &s, uint8_t i)
  { return i < IrrigationSettings::MAX_DIGITAL_IN && s.digitalInMode[i] != InputMode::DISABLED; }
  ```
- Recalcular o `static_assert(sizeof(...))`.

- [ ] **Step 4: Rodar e confirmar verde (e a suíte não quebra em cascata)**

Run: `./bin/run-tests.sh test_irrigation_config`
Expected: PASS. (Outras suítes que usavam os campos removidos serão corrigidas nas tasks seguintes; se o build inteiro quebrar aqui, seguir pro Task 5/6 antes de reexecutar a suíte toda.)

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationSettings.h src/modules/irrigation/IrrigationSettings.cpp test/test_irrigation_config/test_main.cpp
git add src/modules/irrigation/IrrigationSettings.h src/modules/irrigation/IrrigationSettings.cpp test/test_irrigation_config/test_main.cpp
git commit -m "refactor(irrigation): settings ganha digitalInMode[], remove LED dedicado (greenfield)"
```

---

### Task 5: `gwFireRemote` aceita ação explícita

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (assinatura de `gwFireRemote`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp:4475-4493` (`gwFireRemote`), `:4438` (`handleRemoteTrigger` passa `rt.action`)
- Test: `test/test_remote_button_table/test_main.cpp` ou `test/test_irrigation_webapi` (cobertura de resolução de ação)

**Interfaces:**
- Consumes: Task 2 `RemoteTrigger::action`.
- Produces: `void gwFireRemote(uint32_t node, uint8_t inputIdx, uint8_t action);`
  - `action==ACTION_TOGGLE` → resolve via `remoteToggleAction(gwZoneIsOpen(...))`.
  - `action==ACTION_OPEN` → `gwRunCommandOpen`. `action==ACTION_CLOSE` → `gwRunCommandClose`.

- [ ] **Step 1: Escrever o teste que falha**

Num teste de tabela/gateway existente, verificar que, dado `action=OPEN`, a resolução chama abrir independentemente do estado atual (usar o harness de `gwFireRemote` já presente; se não houver, cobrir a função pura de resolução de ação extraída). Mínimo:
```cpp
static void test_action_open_forces_open()
{
    // resolver(action, isOpen) -> comando
    // ACTION_OPEN sempre abre; ACTION_CLOSE sempre fecha; ACTION_TOGGLE inverte
    TEST_ASSERT_EQUAL_UINT8(1, resolveRemoteAction(/*action=*/1, /*isOpen=*/true));  // OPEN
    TEST_ASSERT_EQUAL_UINT8(0, resolveRemoteAction(/*action=*/2, /*isOpen=*/false)); // CLOSE
    TEST_ASSERT_EQUAL_UINT8(0, resolveRemoteAction(/*action=*/0, /*isOpen=*/true));  // TOGGLE->fecha
    TEST_ASSERT_EQUAL_UINT8(1, resolveRemoteAction(/*action=*/0, /*isOpen=*/false)); // TOGGLE->abre
}
```
Extrair helper puro em `RemoteButtonEdge.h` (ou novo header): `inline uint8_t resolveRemoteAction(uint8_t action, bool isOpen)`.

- [ ] **Step 2: Rodar e confirmar que falha**

Run: `./bin/run-tests.sh test_remote_button_table`
Expected: FALHA — `resolveRemoteAction` não existe.

- [ ] **Step 3: Implementar**

Em `RemoteButtonEdge.h` (ou header de protocolo):
```cpp
// action: 0=TOGGLE,1=OPEN,2=CLOSE. Retorna 1=abrir, 0=fechar.
inline uint8_t resolveRemoteAction(uint8_t action, bool isOpen)
{
    if (action == 1) return 1;
    if (action == 2) return 0;
    return isOpen ? 0u : 1u; // TOGGLE
}
```
Em `IrrigationModule.cpp`:
```cpp
void IrrigationModule::gwFireRemote(uint32_t node, uint8_t inputIdx, uint8_t action)
{
    const RemoteAssoc *hits[RemoteButtonTable::MAX];
    size_t k = gateway.remoteButtons.findByTrigger(node, inputIdx, hits, RemoteButtonTable::MAX);
    for (size_t i = 0; i < k; i++) {
        const RemoteAssoc *a = hits[i];
        if (!a->enabled || !gateway.zones.byId(a->targetZoneId))
            continue;
        uint8_t act = resolveRemoteAction(action, gwZoneIsOpen(a->targetZoneId));
        if (act == 1) gwRunCommandOpen(a->targetZoneId);
        else          gwRunCommandClose(a->targetZoneId);
        gwPushRemoteLed();
    }
}
```
E em `handleRemoteTrigger`: `gwFireRemote(mp.from, rt.inputIdx, rt.action);`. Atualizar a declaração no `.h`.

- [ ] **Step 4: Rodar e confirmar verde**

Run: `./bin/run-tests.sh test_remote_button_table`
Expected: PASS.

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp src/modules/irrigation/RemoteButtonEdge.h test/test_remote_button_table/test_main.cpp
git add -A
git commit -m "feat(irrigation): gwFireRemote resolve acao explicita (toggle/open/close)"
```

---

### Task 6: Wiring no módulo — política na fonte + feedback no LED de status

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (campo `InputActuationPolicy _inputPolicy[MAX_DIGITAL_IN]`; remover `_btnEdge`, `_remoteLed`, mirror; campo `uint32_t _cmdFeedbackUntilMs`, `LedPatternController::Mode _cmdFeedbackMode`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` — sampling de entrada (`:1171-1203`, `:3819-3873`), `handleRemoteLed` (`:4407-4419`), ACK P2P (`:427-442`), `refreshLedMode` (`:2016-2032`), `applyLocalRemoteLeds`/`gwPushRemoteLed` (render de LED).

**Interfaces:**
- Consumes: Tasks 1,2,3,5.
- Produces: comportamento runtime — cada entrada avaliada por `_inputPolicy[i]` emitindo `Intent`; estação envia `RemoteTrigger{i, action}`; feedback renderiza em `led` (LedPatternController) via janela transiente.

- [ ] **Step 1: Substituir o sampling de entrada (estação e gateway)**

Nos dois laços de sampling (botoeira local da estação e do gateway, além do bloco de espelho), trocar `RemoteButtonEdge`/`MirrorMode` por `_inputPolicy[i].update(rawHigh, millis())`, mapeando `Intent`→`action` e despachando:
```cpp
for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
    if (settings.pinsDigitalIn[i] < 0 || settings.digitalInMode[i] == InputMode::DISABLED)
        continue;
#ifndef ARCH_PORTDUINO
    bool rawHigh = (digitalRead(settings.pinsDigitalIn[i]) == HIGH);
#else
    bool rawHigh = false;
#endif
    bool activeLow = (settings.digitalInActiveLow >> i) & 1;
    _inputPolicy[i].configure(settings.digitalInMode[i], activeLow); // idempotente; ver nota
    InputActuationPolicy::Intent in = _inputPolicy[i].update(rawHigh, millis());
    if (in == InputActuationPolicy::Intent::NONE)
        continue;
    uint8_t action = (in == InputActuationPolicy::Intent::OPEN) ? 1
                   : (in == InputActuationPolicy::Intent::CLOSE) ? 2 : 0;
    beginCmdFeedback(LedPatternController::Mode::CMD_INFLIGHT); // Step 3
    if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY) {
        gwFireRemote(nodeDB->getNodeNum(), i, action);
    } else if (settings.boundGateway != 0) {
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = settings.boundGateway;
        IrrigationProto::RemoteTrigger rt{i, action};
        p->decoded.payload.size = (uint16_t)IrrigationProto::encodeRemoteTrigger(
            p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, rt);
        if (p->decoded.payload.size) txPacket(p); else packetPool.release(p);
        // fallback P2P permanece, agora carregando `action`
    }
}
```
**Nota `configure` idempotente:** chamar `configure` todo tick reseta o estado do debounce. Em vez disso, só reconfigurar quando `settings.configEpoch` mudar (guardar `_inputPolicyEpoch`); caso contrário manter o estado. Implementar esse guard.

- [ ] **Step 2: Rota de fallback P2P e `_pendingDirect` carregam `action`**

Ajustar o bloco `_fallback.takeExpired` (`:1205-1241`) para despachar com a `action` resolvida (guardar a última `action` por entrada, ou re-resolver). O `_pendingDirect`/ACK passa a disparar `beginCmdFeedback(CMD_OK/CMD_FAIL)` em vez de `_remoteLed[...]`.

- [ ] **Step 3: Feedback transiente no LedPatternController**

Adicionar helper e integrar no `refreshLedMode`:
```cpp
void IrrigationModule::beginCmdFeedback(LedPatternController::Mode m)
{
    _cmdFeedbackMode = m;
    _cmdFeedbackUntilMs = millis() + (m == LedPatternController::Mode::CMD_INFLIGHT ? 7000u : 4000u);
}
```
No topo de `refreshLedMode`, antes da cadeia atual:
```cpp
if ((int32_t)(millis() - _cmdFeedbackUntilMs) < 0) {
    // segurança tem prioridade sobre feedback de comando
    if (!(stationPairing.state() == StationPairing::State::WINDOW || gatewayPairing.windowOpen())) {
        uint16_t vbat = batteryCentiV();
        if (!(vbat != 0 && vbat < settings.vbatMinAbrirCentiV)) {
            led.setMode(_cmdFeedbackMode);
            return;
        }
    }
}
```
`handleRemoteLed` (estação): em vez de `_remoteLed[s].onLedState(...)`, chamar `beginCmdFeedback(anyOn ? CMD_OK : CMD_OK)` — o estado real já está na zona; o importante é sinalizar "confirmou". Em NACK/timeout usar `CMD_FAIL`. Manter `_fallback.clear(i)`.

- [ ] **Step 4: Rodar a suíte inteira**

Run: `./bin/run-tests.sh`
Expected: GREEN. Corrigir qualquer suíte que referencie símbolos removidos (apontar para os novos).

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git add -A
git commit -m "feat(irrigation): politica-na-fonte + feedback de comando no LED de status"
```

---

### Task 7: Remover `MirrorMode`, `RemoteLedFsm`, `Zone::fonteInput` (limpeza greenfield)

**Files:**
- Delete: `src/modules/irrigation/MirrorMode.h`, `src/modules/irrigation/MirrorMode.cpp`, `src/modules/irrigation/RemoteLedFsm.h`
- Delete: `test/test_irrigation_mirror/` (ou reescrever como `InputActuationPolicy` FOLLOW — já coberto no Task 1; remover)
- Modify: `src/modules/irrigation/GatewayTables.h`/`.cpp` (remover `Zone::fonteInput`, `fonteEnabled`, `byFonte`, `mirrorOwnsZoneOutput`)
- Modify: `src/modules/irrigation/IrrigationModule.*` (remover `gateway.mirror`, `gwApplyMirrorMapping`, `gwClearMirrorMapping`, laço de espelho `:3819-3854`, `_remoteLed`, `_ledCache*` se órfão)
- Modify: qualquer `#include` órfão.

**Interfaces:**
- Consumes: Tasks 1,5,6 (o comportamento de espelho agora é `InputActuationPolicy` modo `FOLLOW` + `gwFireRemote`).

- [ ] **Step 1: Apagar os arquivos e o teste de espelho**

```bash
git rm src/modules/irrigation/MirrorMode.h src/modules/irrigation/MirrorMode.cpp src/modules/irrigation/RemoteLedFsm.h
git rm -r test/test_irrigation_mirror
```

- [ ] **Step 2: Remover referências**

Grep e remover usos: `grep -rn "MirrorMode\|RemoteLedFsm\|fonteInput\|fonteEnabled\|byFonte\|mirrorOwnsZoneOutput\|gateway.mirror" src/`. Limpar cada ocorrência (zona-alvo agora vem só da `RemoteButtonTable`).

- [ ] **Step 3: Compilar o firmware alvo**

Run: `./bin/run-tests.sh` (build nativo) — Expected: compila sem símbolos órfãos.
Build de placa (se disponível no ambiente): `pio run -e <env_irrigacao>`.

- [ ] **Step 4: Rodar a suíte inteira**

Run: `./bin/run-tests.sh`
Expected: GREEN.

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/
git add -A
git commit -m "refactor(irrigation): remove MirrorMode/RemoteLedFsm/Zone.fonteInput (absorvidos pela Fonte de Acionamento)"
```

---

### Task 8: Gateway expõe entradas das estações + push de config via `configEpoch`

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.*` (anexar `digitalInMode[4]` + `digitalInActiveLow` ao heartbeat da estação, OU nova msg curta de "input config report")
- Modify: `src/modules/irrigation/StationTelemetryCache.*` / `GatewayTables.*` (cache de entradas por estação)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (`handleGwHeartbeat` guarda o relato; push de `digitalInMode`/polaridade pro nó dono ao editar — reusa o fluxo de config/`configEpoch` existente)
- Test: `test/test_irrigation_gwtables` ou `test/test_irrigation_protocol`

**Interfaces:**
- Consumes: Tasks 2,4.
- Produces: o gateway conhece, por estação, `digitalInMode[i]`+polaridade (p/ o `GET /acionamento` da UI-spec montar o grid agregado). E um caminho de escrita que empurra a config de entrada pra estação dona.

**Nota de escopo:** esta é a task mais pesada (plumbing de mesh). Se o build/tempo apertar, pode virar sub-plano próprio — mas a UI depende dela para o grid agregado. Entregável mínimo: heartbeat da estação carrega os modos; gateway cacheia; endpoint de leitura exposto (a serialização JSON fica na UI-plan).

- [ ] **Step 1: Teste — heartbeat carrega modos de entrada**

Em `test/test_irrigation_protocol`, estender o caso de heartbeat para incluir `digitalInMode[4]`+`digitalInActiveLow` e verificar roundtrip encode/decode.

- [ ] **Step 2: Rodar e confirmar que falha**

Run: `./bin/run-tests.sh test_irrigation_protocol` — Expected: FALHA.

- [ ] **Step 3: Implementar o relato no heartbeat + cache no gateway**

Anexar os campos ao encoder/decoder do heartbeat; `handleGwHeartbeat` grava em `StationTelemetryCache` (ou struct nova `StationInputs`). Implementar o push: ao gravar config de entrada de uma estação no gateway, incrementar `configEpoch` e enviar o comando de config existente pra ela.

- [ ] **Step 4: Rodar a suíte inteira**

Run: `./bin/run-tests.sh` — Expected: GREEN.

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/
git add -A
git commit -m "feat(irrigation): gateway cacheia entradas das estacoes + push de config via configEpoch"
```

---

## Self-Review (feito pelo autor do plano)

**Cobertura da spec:**
- Política na fonte → Task 1,6. ✓
- Enum 5+1 modos → Task 1,4. ✓
- Renovação 60 s FOLLOW → Task 1. ✓
- Protocolo `action` → Task 2,5. ✓
- Feedback no LED de status → Task 3,6. ✓
- Remove LED dedicado/MirrorMode/fonteInput → Task 4,7. ✓
- Greenfield (sem migração) → Task 4. ✓
- Deps de UI (expor entradas + push config) → Task 8. ✓

**Placeholders:** código real em cada step. Task 6/7/8 têm pontos "ajustar ao real" porque dependem de trechos grandes do `IrrigationModule.cpp` — o implementador deve ler as linhas citadas antes de editar.

**Consistência de tipos:** `Intent{NONE,OPEN,CLOSE,TOGGLE}`, `action{0=TOGGLE,1=OPEN,2=CLOSE}`, `resolveRemoteAction(action,isOpen)→1 abrir/0 fechar`, `digitalInMode[]:InputMode` — usados de forma idêntica entre as tasks.
