# Irrigação Mesh — Fase 3: Pareamento, vínculo, reset de fábrica, botão e LED

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adoção de estação nova sem ferramenta externa: botão físico abre janela de 2 min → `PAIR_ANNOUNCE` → gateway (com janela de aceitação aberta pelo próprio botão) responde `PAIR_GRANT` com credenciais da fazenda → estação grava PSK+vínculo atomicamente e reinicia no canal da fazenda; allowlist no gateway; reset de fábrica por botão de 10 s; LED de status com tabela única de padrões; acionamento manual e teste de pulso pelo botão.

**Architecture:** Toda a lógica nova é pura e testável (ButtonGestureDetector, LedPatternController, StationPairing, GatewayPairing, Allowlist, codecs). A camada de integração (Task 5) liga: uma thread de UI de 25 ms (GPIO do botão + LED), os handlers de rádio no IrrigationModule, escrita de canal via `Channels::setChannel`+`onConfigChanged`, e reboot via `rebootAtMsec`. Aprovação de pareamento nesta fase = janela física no gateway (botão), documentada como antecipação do painel da Fase 5.

**Tech Stack:** C++17, Unity nativo em Docker (comando com volumes `pio-cache`/`fwtest-cache`), módulos das Fases 1–2 em `src/modules/irrigation/`, `src/mesh/Channels.h`, `rebootAtMsec` (`src/main.h`).

## Global Constraints

- **Janela de pareamento: 2 min** (spec §6/§10); anúncio a cada 10 s enquanto aberta.
- **Reset de fábrica: botão 10 s** (spec §6/§10), com confirmação por padrão de LED; apaga settings e allowlist e reinicia.
- Nó de fábrica (**`boundGateway == 0`**): **qualquer pressão** abre a janela de pareamento (§8.6); não executa gestos de válvula.
- Gestos (§8.6, nó pareado): curta <1 s = portal (Fase 5 — nesta fase só log + LED); dupla (gap ≤400 ms) = acionamento manual da válvula 0 (abre `DEFAULT_MANUAL_OPEN_S = 20*60` s — `padrao_min` do exemplo §5.2 — nova dupla fecha); longa 3 s = teste de pulso (abre 10 s, fecha automático); 10 s = reset de fábrica.
- LED (§8.7) — **tabela única no firmware**: 1 piscada/5 s normal; 2/5 s sem contato com gateway; 3/5 s config pendente **ou modo seguro**; piscada rápida contínua = janela de pareamento/portal; aceso fixo = saída aberta; SOS = bateria crítica. Prioridade (decisão de projeto, documentar no código): PAIRING > BATTERY_SOS > OUTPUT_OPEN > CONFIG_PENDING > NO_GATEWAY > NORMAL.
- `PAIR_GRANT` carrega **PSK de 32 bytes + nome do canal (≤11 chars) + node id do gateway**; trafega no canal corrente (chave conhecida/default de fábrica — exposição momentânea aceita pela spec §6, janela de 2 min + botão físico + aprovação física no gateway).
- Config/credenciais: commit atômico antes do reboot (staged write existente); `boundGateway` só muda via pareamento ou reset — nunca via `SET_CONFIG` (invariante das Fases 1–2 mantido).
- Todo acionamento manual/pareamento/reset gera **`EVENTO`** (§8.6) — codec nesta fase; consumo/auditoria no gateway = Fases 4/6.
- Settings **v3** (mesmo sizeof 52): `pinBtn` e `pinLed` (i8, -1 = ausente) entram no lugar de 2 bytes do pad final; migração v1/v2→v3.
- Anti-replay e rate limit continuam valendo para mensagens de pareamento.
- Estilo: LOG_* com node IDs `0x%08x`; comentários mínimos; C++17; nada em `src/mesh/generated/`; testes com `#include "Arduino.h"` como PRIMEIRO include; `test/native-suite-count` **36 → 38** (Task 5, nunca antes).
- Teste no host (Docker, template — trocar `<SUITE>` e o nome do container por task):

```
MSYS_NO_PATHCONV=1 docker run --rm --name irrig-p3tN -u 0 -e HOME=/root -v "$(pwd -W 2>/dev/null || pwd):/src:ro" -v pio-cache:/root -v fwtest-cache:/tmp/fw-test meshtastic-native-test timeout -k 30 1500 bash -c "cp -a /src/src/. /tmp/fw-test/src/ && cp -a /src/test/. /tmp/fw-test/test/ && cd /tmp/fw-test && platformio test -e coverage -f <SUITE> -vv 2>&1 | grep -E ':PASS|:FAIL|PASSED|FAILED|ERRORED|undefined|error' | head -40"
```

## File Structure

```
src/modules/irrigation/
  IrrigationProtocol.h/.cpp     # MODIFY: codecs PAIR_ANNOUNCE/PAIR_GRANT/EVENTO (Task 1)
  IrrigationSettings.h/.cpp     # MODIFY: v3 pinBtn/pinLed (Task 2)
  ButtonGesture.h/.cpp          # CREATE: detector de gestos puro (Task 3)
  LedPattern.h/.cpp             # CREATE: padrões de LED puros, tabela única (Task 3)
  Pairing.h/.cpp                # CREATE: StationPairing + GatewayPairing puros (Task 4)
  Allowlist.h/.cpp              # CREATE: allowlist com (de)serialização (Task 4)
  IrrigationModule.h/.cpp       # MODIFY: handlers pareamento, thread UI, reset, canal (Task 5)
test/test_irrigation_protocol/test_main.cpp  # MODIFY (Task 1)
test/test_irrigation_config/test_main.cpp    # MODIFY (Task 2)
test/test_irrigation_ui/test_main.cpp        # CREATE: botão+LED (Task 3)
test/test_irrigation_pairing/test_main.cpp   # CREATE: pareamento+allowlist (Task 4)
test/native-suite-count                      # 36 → 38 (Task 5)
```

---

### Task 1: Codecs `PAIR_ANNOUNCE`, `PAIR_GRANT`, `EVENTO`

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h`
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp`
- Test: `test/test_irrigation_protocol/test_main.cpp` (estender; hoje 11 testes)

**Interfaces:**
- Consumes: Writer/Reader/writeHeader/bodyReader existentes; `MSG_PAIR_ANNOUNCE=8`, `MSG_PAIR_GRANT=9`, `MSG_EVENTO=7` (já no enum).
- Produces:

```cpp
struct PairAnnounce { uint8_t protoVersion; uint8_t nameLen; char name[16]; };
struct PairGrant { uint8_t psk[32]; uint8_t nameLen; char channelName[12]; uint32_t gatewayId; };
enum EventCode : uint8_t { EV_MANUAL_OPEN = 1, EV_MANUAL_CLOSE = 2, EV_TEST_PULSE = 3, EV_PAIRED = 4, EV_FACTORY_RESET = 5 };
struct Evento { uint8_t code; uint32_t arg; };
size_t encodePairAnnounce(uint8_t *buf, size_t len, uint32_t seq, const PairAnnounce &m);
bool decodePairAnnounce(const uint8_t *buf, size_t len, PairAnnounce &out);
size_t encodePairGrant(uint8_t *buf, size_t len, uint32_t seq, const PairGrant &m);
bool decodePairGrant(const uint8_t *buf, size_t len, PairGrant &out);
size_t encodeEvento(uint8_t *buf, size_t len, uint32_t seq, const Evento &m);
bool decodeEvento(const uint8_t *buf, size_t len, Evento &out);
```

Regras: `nameLen` ≤ 15 (announce) / ≤ 11 (grant); encode rejeita (retorna 0) acima; decode rejeita e garante NUL-termination de `name`/`channelName` no struct de saída.

- [ ] **Step 1: Testes que falham** — acrescentar a `test/test_irrigation_protocol/test_main.cpp` (+RUN_TEST):

```cpp
static void test_pairAnnounce_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    PairAnnounce in = {};
    in.protoVersion = VERSION;
    in.nameLen = 5;
    memcpy(in.name, "Pasto", 5);
    size_t n = encodePairAnnounce(buf, sizeof(buf), 2, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_PAIR_ANNOUNCE, h.type);
    PairAnnounce out;
    TEST_ASSERT_TRUE(decodePairAnnounce(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(VERSION, out.protoVersion);
    TEST_ASSERT_EQUAL_UINT8(5, out.nameLen);
    TEST_ASSERT_EQUAL_STRING("Pasto", out.name);
}

static void test_pairGrant_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    PairGrant in = {};
    for (int i = 0; i < 32; i++)
        in.psk[i] = (uint8_t)(i * 3);
    in.nameLen = 7;
    memcpy(in.channelName, "bvirrig", 7);
    in.gatewayId = 0xa1b2c3d4;
    size_t n = encodePairGrant(buf, sizeof(buf), 3, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    PairGrant out;
    TEST_ASSERT_TRUE(decodePairGrant(buf, n, out));
    TEST_ASSERT_EQUAL_MEMORY(in.psk, out.psk, 32);
    TEST_ASSERT_EQUAL_STRING("bvirrig", out.channelName);
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, out.gatewayId);
}

static void test_pairGrant_nameTooLong_rejected()
{
    uint8_t buf[MAX_PAYLOAD];
    PairGrant in = {};
    in.nameLen = 12; // máx 11
    TEST_ASSERT_EQUAL_UINT(0, encodePairGrant(buf, sizeof(buf), 1, in));
}

static void test_evento_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    Evento in = {EV_MANUAL_OPEN, 1200};
    size_t n = encodeEvento(buf, sizeof(buf), 4, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    Evento out;
    TEST_ASSERT_TRUE(decodeEvento(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(EV_MANUAL_OPEN, out.code);
    TEST_ASSERT_EQUAL_UINT32(1200, out.arg);
}

static void test_pairGrant_truncated_rejected()
{
    uint8_t buf[MAX_PAYLOAD];
    PairGrant in = {};
    in.nameLen = 4;
    memcpy(in.channelName, "abcd", 4);
    in.gatewayId = 1;
    size_t n = encodePairGrant(buf, sizeof(buf), 1, in);
    PairGrant out;
    TEST_ASSERT_FALSE(decodePairGrant(buf, n - 1, out));
}
```

- [ ] **Step 2: RED** — Run suite `test_irrigation_protocol`. Expected: FAIL de compilação (`PairAnnounce` inexistente).

- [ ] **Step 3: Implementar** — em `IrrigationProtocol.h` (dentro do namespace, após `SetConfig`), os structs/enum/funções acima. Em `IrrigationProtocol.cpp`:

```cpp
size_t encodePairAnnounce(uint8_t *buf, size_t len, uint32_t seq, const PairAnnounce &m)
{
    if (m.nameLen > 15)
        return 0;
    Writer w{buf, len};
    writeHeader(w, MSG_PAIR_ANNOUNCE, seq);
    w.u8(m.protoVersion);
    w.u8(m.nameLen);
    for (uint8_t i = 0; w.ok && i < m.nameLen; i++)
        w.u8((uint8_t)m.name[i]);
    return w.ok ? w.pos : 0;
}

bool decodePairAnnounce(const uint8_t *buf, size_t len, PairAnnounce &out)
{
    Reader r = bodyReader(buf, len);
    out.protoVersion = r.u8();
    out.nameLen = r.u8();
    if (!r.ok || out.nameLen > 15 || r.pos + out.nameLen > len)
        return false;
    memcpy(out.name, buf + r.pos, out.nameLen);
    out.name[out.nameLen] = '\0';
    return true;
}

size_t encodePairGrant(uint8_t *buf, size_t len, uint32_t seq, const PairGrant &m)
{
    if (m.nameLen > 11)
        return 0;
    Writer w{buf, len};
    writeHeader(w, MSG_PAIR_GRANT, seq);
    for (int i = 0; w.ok && i < 32; i++)
        w.u8(m.psk[i]);
    w.u8(m.nameLen);
    for (uint8_t i = 0; w.ok && i < m.nameLen; i++)
        w.u8((uint8_t)m.channelName[i]);
    w.u32(m.gatewayId);
    return w.ok ? w.pos : 0;
}

bool decodePairGrant(const uint8_t *buf, size_t len, PairGrant &out)
{
    Reader r = bodyReader(buf, len);
    for (int i = 0; i < 32; i++)
        out.psk[i] = r.u8();
    out.nameLen = r.u8();
    if (!r.ok || out.nameLen > 11 || r.pos + out.nameLen + 4 > len)
        return false;
    memcpy(out.channelName, buf + r.pos, out.nameLen);
    out.channelName[out.nameLen] = '\0';
    r.pos += out.nameLen;
    out.gatewayId = r.u32();
    return r.ok;
}

size_t encodeEvento(uint8_t *buf, size_t len, uint32_t seq, const Evento &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_EVENTO, seq);
    w.u8(m.code);
    w.u32(m.arg);
    return w.ok ? w.pos : 0;
}

bool decodeEvento(const uint8_t *buf, size_t len, Evento &out)
{
    Reader r = bodyReader(buf, len);
    out.code = r.u8();
    out.arg = r.u32();
    return r.ok;
}
```

Nota ao executor: `name[16]`/`channelName[12]` comportam o NUL (`nameLen` máx 15/11).

- [ ] **Step 4: GREEN** — Run suite `test_irrigation_protocol`. Expected: PASS (16 testes: 11 + 5).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.* test/test_irrigation_protocol/
git commit -m "feat(irrigation): add pairing and event message codecs"
```

---

### Task 2: Settings v3 — pinos de botão e LED

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h`
- Modify: `src/modules/irrigation/IrrigationSettings.cpp`
- Test: `test/test_irrigation_config/test_main.cpp` (estender; hoje 5 testes)

**Interfaces:**
- Consumes: struct v2 (52 B, `pad2[3]` no fim), `migrateIrrigationSettings`.
- Produces: struct v3 — `pad2[3]` vira `int8_t pinBtn = -1; int8_t pinLed = -1; uint8_t pad2 = 0;` (offsets 49/50/51 — **sizeof continua 52**); `version = 3`; migração aceita v1 (40 B), v2 (52 B, zera→pinos default -1) e v3 (52 B). Distinção v2/v3 pelo campo `version`.

- [ ] **Step 1: Testes que falham** — acrescentar a `test/test_irrigation_config/test_main.cpp` (+RUN_TEST):

```cpp
static void test_migrate_v2_getsDefaultButtonLedPins()
{
    IrrigationSettings v2like;
    v2like.version = 2; // simula blob v2: mesmos 52 bytes, pinos ainda não existiam
    v2like.pinBtn = 0;  // lixo nos bytes que eram pad no v2
    v2like.pinLed = 0;
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings((const uint8_t *)&v2like, sizeof(v2like), out));
    TEST_ASSERT_EQUAL_UINT16(3, out.version);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinBtn); // v2 não tinha o campo: default
    TEST_ASSERT_EQUAL_INT8(-1, out.pinLed);
}

static void test_migrate_v3_passthroughKeepsPins()
{
    IrrigationSettings in;
    in.pinBtn = 0;
    in.pinLed = 2;
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings((const uint8_t *)&in, sizeof(in), out));
    TEST_ASSERT_EQUAL_UINT16(3, out.version);
    TEST_ASSERT_EQUAL_INT8(0, out.pinBtn);
    TEST_ASSERT_EQUAL_INT8(2, out.pinLed);
}

static void test_migrate_v1_getsDefaultButtonLedPins()
{
    uint8_t img[IRRIGATION_SETTINGS_V1_SIZE];
    buildV1Image(img);
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(img, sizeof(img), out));
    TEST_ASSERT_EQUAL_UINT16(3, out.version);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinBtn);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinLed);
}
```

E ajustar os testes existentes que assertavam `version == 2` (migração v1 e passthrough) para `3`.

- [ ] **Step 2: RED** — Run suite `test_irrigation_config`. Expected: FAIL de compilação (`pinBtn` inexistente).

- [ ] **Step 3: Implementar** — no header: `uint16_t version = 3;`, trocar `uint8_t pad2[3] = {0,0,0};` por:

```cpp
    int8_t pinBtn = -1; // botão multifunção (§8.6); -1 = ausente
    int8_t pinLed = -1; // LED de status (§8.7); -1 = ausente
    uint8_t pad2 = 0;
```

(atualizar comentário de layout; `static_assert` continua 52). Na migração:

```cpp
    if (version == 3) {
        if (n != sizeof(IrrigationSettings))
            return false;
        memcpy(&out, raw, sizeof(out));
        return true;
    }
    if (version == 2) {
        if (n != sizeof(IrrigationSettings))
            return false;
        IrrigationSettings s;
        memcpy(&s, raw, sizeof(s));
        s.version = 3;
        s.pinBtn = -1; // bytes eram padding no v2
        s.pinLed = -1;
        s.pad2 = 0;
        out = s;
        return true;
    }
    if (version == 1) { /* como hoje, mas s.version = 3 */ }
```

E no load, regravar quando `migrated = (n == IRRIGATION_SETTINGS_V1_SIZE) || versão lida < 3` — ler a versão do blob antes: mais simples, comparar `tmp.version` pós-migração é sempre 3; detectar upgrade por `n == 40 || raw version != 3` (ler `raw[4..5]` com memcpy).

- [ ] **Step 4: GREEN** — Run suite `test_irrigation_config`. Expected: PASS (8 testes: 5 ajustados + 3 novos).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationSettings.* test/test_irrigation_config/
git commit -m "feat(irrigation): settings v3 with button and led pins"
```

---

### Task 3: `ButtonGestureDetector` + `LedPatternController` (tabela única §8.7)

**Files:**
- Create: `src/modules/irrigation/ButtonGesture.h` / `.cpp`
- Create: `src/modules/irrigation/LedPattern.h` / `.cpp`
- Test: `test/test_irrigation_ui/test_main.cpp` (nova suite)

**Interfaces:**
- Produces:

```cpp
class ButtonGestureDetector
{
  public:
    enum class Event : uint8_t { NONE, SHORT, DOUBLE, LONG_3S, HOLD_10S };
    static constexpr uint32_t DEBOUNCE_MS = 30, DOUBLE_GAP_MS = 400, SHORT_MAX_MS = 1000;
    static constexpr uint32_t LONG_MS = 3000, HOLD_MS = 10000;
    Event update(bool pressed, uint32_t nowMs); // chamar a cada amostra (~25 ms)
};

class LedPatternController
{
  public:
    enum class Mode : uint8_t { NORMAL, NO_GATEWAY, CONFIG_PENDING, PAIRING, OUTPUT_OPEN, BATTERY_SOS };
    void setMode(Mode m) { mode = m; }
    Mode currentMode() const { return mode; }
    bool ledOn(uint32_t nowMs) const; // função pura do tempo
  private:
    Mode mode = Mode::NORMAL;
};
```

Semântica do detector: pressão registrada com debounce de 30 ms; `LONG_3S` dispara UMA vez aos 3 s de pressão contínua (antes de soltar); `HOLD_10S` dispara UMA vez aos 10 s (mesma pressão — quem consumiu LONG_3S ainda recebe HOLD_10S depois; o glue decide o que fazer); soltar antes de 1 s arma `SHORT`, que só é emitido se NÃO vier segunda pressão em 400 ms; segunda pressão dentro do gap emite `DOUBLE` imediatamente.

Padrões do LED (tabela única, §8.7): janela de 5000 ms; piscadas de 100 ms com gap de 150 ms no início da janela (contagem 1/2/3 conforme modo); `PAIRING` = 100 ms on / 100 ms off contínuo; `OUTPUT_OPEN` = sempre on; `BATTERY_SOS` = padrão morse `...---...`: 3×(150 on/150 off), 3×(450 on/150 off), 3×(150 on/150 off), pausa até completar 5000 ms.

- [ ] **Step 1: Testes que falham** — `test/test_irrigation_ui/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ButtonGesture.h"
#include "modules/irrigation/LedPattern.h"
#include <unity.h>

using Ev = ButtonGestureDetector::Event;
using Mode = LedPatternController::Mode;

void setUp(void) {}
void tearDown(void) {}

// Simula amostragem de 25 ms: pressiona em [t0, t1), retorna eventos coletados.
static void sample(ButtonGestureDetector &d, uint32_t from, uint32_t to, bool pressed, Ev *outEv, int &count)
{
    for (uint32_t t = from; t < to; t += 25) {
        Ev e = d.update(pressed, t);
        if (e != Ev::NONE && count < 8)
            outEv[count++] = e;
    }
}

static void test_shortPress_emitsShortAfterGap()
{
    ButtonGestureDetector d;
    Ev evs[8];
    int n = 0;
    sample(d, 0, 200, true, evs, n);    // pressiona 200 ms
    sample(d, 200, 700, false, evs, n); // solta; gap de dupla expira em 600
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(Ev::SHORT, evs[0]);
}

static void test_doublePress_emitsDouble()
{
    ButtonGestureDetector d;
    Ev evs[8];
    int n = 0;
    sample(d, 0, 150, true, evs, n);
    sample(d, 150, 300, false, evs, n); // solta 150 ms
    sample(d, 300, 450, true, evs, n);  // segunda pressão dentro dos 400 ms
    sample(d, 450, 1000, false, evs, n);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(Ev::DOUBLE, evs[0]);
}

static void test_longPress_firesOnceAt3s_thenHoldAt10s()
{
    ButtonGestureDetector d;
    Ev evs[8];
    int n = 0;
    sample(d, 0, 11000, true, evs, n); // segura 11 s
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL(Ev::LONG_3S, evs[0]);
    TEST_ASSERT_EQUAL(Ev::HOLD_10S, evs[1]);
    sample(d, 11000, 12000, false, evs, n); // soltar não emite mais nada
    TEST_ASSERT_EQUAL_INT(2, n);
}

static void test_bounceIgnored()
{
    ButtonGestureDetector d;
    Ev evs[8];
    int n = 0;
    // pulso de 25 ms (uma amostra) — abaixo do debounce de 30 ms
    d.update(true, 0);
    sample(d, 25, 800, false, evs, n);
    TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_led_normal_oneBlinkPer5s()
{
    LedPatternController led;
    led.setMode(Mode::NORMAL);
    TEST_ASSERT_TRUE(led.ledOn(5000 + 50));    // 1ª piscada da janela
    TEST_ASSERT_FALSE(led.ledOn(5000 + 200));  // gap
    TEST_ASSERT_FALSE(led.ledOn(5000 + 300));  // sem 2ª piscada
    TEST_ASSERT_FALSE(led.ledOn(5000 + 4000)); // resto da janela apagado
}

static void test_led_noGateway_twoBlinks_configPending_three()
{
    LedPatternController led;
    led.setMode(Mode::NO_GATEWAY);
    TEST_ASSERT_TRUE(led.ledOn(50));   // piscada 1 [0,100)
    TEST_ASSERT_TRUE(led.ledOn(300));  // piscada 2 [250,350)
    TEST_ASSERT_FALSE(led.ledOn(550)); // sem piscada 3
    led.setMode(Mode::CONFIG_PENDING);
    TEST_ASSERT_TRUE(led.ledOn(550)); // piscada 3 [500,600)
}

static void test_led_pairing_fastBlink_and_open_solid()
{
    LedPatternController led;
    led.setMode(Mode::PAIRING);
    TEST_ASSERT_TRUE(led.ledOn(50));   // [0,100) on
    TEST_ASSERT_FALSE(led.ledOn(150)); // [100,200) off
    TEST_ASSERT_TRUE(led.ledOn(250));
    led.setMode(Mode::OUTPUT_OPEN);
    TEST_ASSERT_TRUE(led.ledOn(0));
    TEST_ASSERT_TRUE(led.ledOn(123456));
}

static void test_led_sos_shortShortLong()
{
    LedPatternController led;
    led.setMode(Mode::BATTERY_SOS);
    TEST_ASSERT_TRUE(led.ledOn(50));    // ponto 1 [0,150)
    TEST_ASSERT_FALSE(led.ledOn(200));  // gap
    TEST_ASSERT_TRUE(led.ledOn(350));   // ponto 2 [300,450)
    TEST_ASSERT_TRUE(led.ledOn(1000));  // traço 1 [900,1350)
    TEST_ASSERT_FALSE(led.ledOn(4500)); // pausa final da janela
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_shortPress_emitsShortAfterGap);
    RUN_TEST(test_doublePress_emitsDouble);
    RUN_TEST(test_longPress_firesOnceAt3s_thenHoldAt10s);
    RUN_TEST(test_bounceIgnored);
    RUN_TEST(test_led_normal_oneBlinkPer5s);
    RUN_TEST(test_led_noGateway_twoBlinks_configPending_three);
    RUN_TEST(test_led_pairing_fastBlink_and_open_solid);
    RUN_TEST(test_led_sos_shortShortLong);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: RED** — Run suite `test_irrigation_ui`. Expected: FAIL de compilação.

- [ ] **Step 3: Implementar**

`ButtonGesture.h`:

```cpp
#pragma once
#include <stdint.h>

// Detector de gestos do botão multifunção (spec §8.6). Alimentar com amostras
// periódicas (~25 ms); eventos disparam no momento correto da linha do tempo.
class ButtonGestureDetector
{
  public:
    enum class Event : uint8_t { NONE, SHORT, DOUBLE, LONG_3S, HOLD_10S };
    static constexpr uint32_t DEBOUNCE_MS = 30, DOUBLE_GAP_MS = 400, SHORT_MAX_MS = 1000;
    static constexpr uint32_t LONG_MS = 3000, HOLD_MS = 10000;

    Event update(bool pressed, uint32_t nowMs);

  private:
    bool stable = false;       // estado debounced
    bool rawLast = false;
    uint32_t rawSinceMs = 0;   // desde quando o estado cru está estável
    uint32_t pressStartMs = 0;
    bool longFired = false;
    bool holdFired = false;
    bool shortArmed = false;   // soltou <1 s; aguardando gap de dupla
    uint32_t shortArmedMs = 0;
};
```

`ButtonGesture.cpp`:

```cpp
#include "ButtonGesture.h"

ButtonGestureDetector::Event ButtonGestureDetector::update(bool pressed, uint32_t nowMs)
{
    if (pressed != rawLast) {
        rawLast = pressed;
        rawSinceMs = nowMs;
    }
    bool debounced = stable;
    if ((nowMs - rawSinceMs) >= DEBOUNCE_MS)
        debounced = rawLast;

    Event ev = Event::NONE;

    if (debounced && !stable) { // borda de pressão
        if (shortArmed && (nowMs - shortArmedMs) <= DOUBLE_GAP_MS) {
            shortArmed = false;
            ev = Event::DOUBLE;
        }
        pressStartMs = nowMs;
        longFired = holdFired = false;
    } else if (!debounced && stable) { // borda de soltura
        uint32_t held = nowMs - pressStartMs;
        if (!longFired && !holdFired && held < SHORT_MAX_MS && ev == Event::NONE) {
            shortArmed = true;
            shortArmedMs = nowMs;
        }
    } else if (debounced) { // segurando
        uint32_t held = nowMs - pressStartMs;
        if (!longFired && held >= LONG_MS) {
            longFired = true;
            ev = Event::LONG_3S;
        } else if (!holdFired && held >= HOLD_MS) {
            holdFired = true;
            ev = Event::HOLD_10S;
        }
    } else { // solto
        if (shortArmed && (nowMs - shortArmedMs) > DOUBLE_GAP_MS) {
            shortArmed = false;
            ev = Event::SHORT;
        }
    }
    stable = debounced;
    return ev;
}
```

Nota ao executor: aos 10 s a condição `held >= LONG_MS` já disparou (`longFired`), então o ramo `else if` do HOLD é alcançado — verifique com o teste; se o HOLD não disparar, inverta a ordem dos dois `if`s (HOLD primeiro).

`LedPattern.h`:

```cpp
#pragma once
#include <stdint.h>

// Tabela ÚNICA dos padrões de LED (spec §8.7). ledOn é função pura do tempo:
// sem estado além do modo — fácil de revisar e de testar.
class LedPatternController
{
  public:
    enum class Mode : uint8_t { NORMAL, NO_GATEWAY, CONFIG_PENDING, PAIRING, OUTPUT_OPEN, BATTERY_SOS };
    void setMode(Mode m) { mode = m; }
    Mode currentMode() const { return mode; }
    bool ledOn(uint32_t nowMs) const;

  private:
    Mode mode = Mode::NORMAL;
};
```

`LedPattern.cpp`:

```cpp
#include "LedPattern.h"

namespace
{
constexpr uint32_t WINDOW_MS = 5000;
constexpr uint32_t BLINK_ON_MS = 100, BLINK_PERIOD_MS = 250; // on 100, gap 150

bool blinkPattern(uint32_t t, uint8_t count)
{
    if (t >= (uint32_t)count * BLINK_PERIOD_MS)
        return false;
    return (t % BLINK_PERIOD_MS) < BLINK_ON_MS;
}

// SOS: 3 pontos (150/150), 3 traços (450/150), 3 pontos (150/150)
bool sosPattern(uint32_t t)
{
    struct Seg {
        uint32_t on, off;
        uint8_t reps;
    };
    static const Seg segs[] = {{150, 150, 3}, {450, 150, 3}, {150, 150, 3}};
    for (const auto &s : segs) {
        for (uint8_t r = 0; r < s.reps; r++) {
            if (t < s.on)
                return true;
            t -= s.on;
            if (t < s.off)
                return false;
            t -= s.off;
        }
    }
    return false; // pausa até o fim da janela
}
} // namespace

bool LedPatternController::ledOn(uint32_t nowMs) const
{
    uint32_t t = nowMs % WINDOW_MS;
    switch (mode) {
    case Mode::NORMAL:
        return blinkPattern(t, 1);
    case Mode::NO_GATEWAY:
        return blinkPattern(t, 2);
    case Mode::CONFIG_PENDING:
        return blinkPattern(t, 3);
    case Mode::PAIRING:
        return (nowMs % 200) < 100;
    case Mode::OUTPUT_OPEN:
        return true;
    case Mode::BATTERY_SOS:
        return sosPattern(t);
    }
    return false;
}
```

- [ ] **Step 4: GREEN** — Run suite `test_irrigation_ui`. Expected: PASS (8 testes).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/ButtonGesture.* src/modules/irrigation/LedPattern.* test/test_irrigation_ui/
git commit -m "feat(irrigation): add button gesture detector and led pattern table"
```

---

### Task 4: `StationPairing` + `GatewayPairing` + `Allowlist`

**Files:**
- Create: `src/modules/irrigation/Pairing.h` / `.cpp`
- Create: `src/modules/irrigation/Allowlist.h` / `.cpp`
- Test: `test/test_irrigation_pairing/test_main.cpp` (nova suite)

**Interfaces:**
- Consumes: `IrrigationProto::PairGrant` (Task 1).
- Produces:

```cpp
class StationPairing
{
  public:
    enum class State : uint8_t { IDLE, WINDOW, COMMITTED };
    static constexpr uint32_t WINDOW_MS = 2 * 60 * 1000, ANNOUNCE_INTERVAL_MS = 10 * 1000;
    void openWindow(uint32_t nowMs);                              // (re)abre janela de 2 min
    void tick(uint32_t nowMs);                                    // expira janela
    bool announceDue(uint32_t nowMs);                             // true = enviar PAIR_ANNOUNCE agora
    bool onGrant(const IrrigationProto::PairGrant &g, uint32_t nowMs); // true = aceito → COMMITTED
    State state() const;
    const IrrigationProto::PairGrant &grant() const;              // válido em COMMITTED
};

class GatewayPairing
{
  public:
    static constexpr uint32_t WINDOW_MS = 2 * 60 * 1000, REGRANT_COOLDOWN_MS = 5 * 1000;
    void openWindow(uint32_t nowMs);
    void tick(uint32_t nowMs);
    bool windowOpen() const;
    bool approveAnnounce(uint32_t nodeId, uint32_t nowMs); // true = enviar PAIR_GRANT (cooldown por nó)
};

class Allowlist
{
  public:
    static constexpr size_t MAX = 16;
    static constexpr uint32_t MAGIC = 0x49414C31; // "IAL1"
    bool add(uint32_t nodeId);           // idempotente; false = cheia
    bool remove(uint32_t nodeId);
    bool contains(uint32_t nodeId) const;
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;      // magic+ver+count+ids; 0 = cap pequena
    bool deserialize(const uint8_t *buf, size_t n);        // false = inválido (lista fica vazia)
};
```

Regras: `announceDue` só em WINDOW e a cada 10 s (primeiro anúncio imediato ao abrir); `onGrant` fora da janela = false (ignorar); `approveAnnounce` false fora da janela; reanúncio do mesmo nó dentro de 5 s não gera novo grant (cooldown anti-flood), nó diferente aprova independente.

- [ ] **Step 1: Testes que falham** — `test/test_irrigation_pairing/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/Allowlist.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "modules/irrigation/Pairing.h"
#include <string.h>
#include <unity.h>

using State = StationPairing::State;

void setUp(void) {}
void tearDown(void) {}

static IrrigationProto::PairGrant makeGrant()
{
    IrrigationProto::PairGrant g = {};
    for (int i = 0; i < 32; i++)
        g.psk[i] = (uint8_t)i;
    g.nameLen = 4;
    memcpy(g.channelName, "farm", 5);
    g.gatewayId = 0xa1b2c3d4;
    return g;
}

static void test_station_windowLifecycleAndAnnounceCadence()
{
    StationPairing sp;
    TEST_ASSERT_EQUAL(State::IDLE, sp.state());
    TEST_ASSERT_FALSE(sp.announceDue(0));

    sp.openWindow(1000);
    TEST_ASSERT_EQUAL(State::WINDOW, sp.state());
    TEST_ASSERT_TRUE(sp.announceDue(1000));   // primeiro imediato
    TEST_ASSERT_FALSE(sp.announceDue(5000));  // <10 s
    TEST_ASSERT_TRUE(sp.announceDue(11000));  // 10 s depois
    sp.tick(1000 + 2 * 60 * 1000 + 1);        // janela expira
    TEST_ASSERT_EQUAL(State::IDLE, sp.state());
    TEST_ASSERT_FALSE(sp.announceDue(200000));
}

static void test_station_grantOnlyInsideWindow()
{
    StationPairing sp;
    TEST_ASSERT_FALSE(sp.onGrant(makeGrant(), 0)); // IDLE: ignora
    sp.openWindow(1000);
    TEST_ASSERT_TRUE(sp.onGrant(makeGrant(), 2000));
    TEST_ASSERT_EQUAL(State::COMMITTED, sp.state());
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, sp.grant().gatewayId);
    TEST_ASSERT_FALSE(sp.announceDue(3000)); // após commit, para de anunciar
}

static void test_gateway_windowAndCooldown()
{
    GatewayPairing gp;
    TEST_ASSERT_FALSE(gp.approveAnnounce(0x11, 0)); // janela fechada
    gp.openWindow(1000);
    TEST_ASSERT_TRUE(gp.windowOpen());
    TEST_ASSERT_TRUE(gp.approveAnnounce(0x11, 2000));
    TEST_ASSERT_FALSE(gp.approveAnnounce(0x11, 4000)); // cooldown 5 s
    TEST_ASSERT_TRUE(gp.approveAnnounce(0x22, 4000));  // outro nó, independente
    TEST_ASSERT_TRUE(gp.approveAnnounce(0x11, 8000));  // cooldown venceu
    gp.tick(1000 + 2 * 60 * 1000 + 1);
    TEST_ASSERT_FALSE(gp.windowOpen());
    TEST_ASSERT_FALSE(gp.approveAnnounce(0x33, 200000));
}

static void test_allowlist_addRemoveContainsIdempotent()
{
    Allowlist al;
    TEST_ASSERT_TRUE(al.add(0x11));
    TEST_ASSERT_TRUE(al.add(0x11)); // idempotente
    TEST_ASSERT_EQUAL_UINT(1, al.count());
    TEST_ASSERT_TRUE(al.contains(0x11));
    TEST_ASSERT_TRUE(al.remove(0x11));
    TEST_ASSERT_FALSE(al.contains(0x11));
    TEST_ASSERT_FALSE(al.remove(0x11));
}

static void test_allowlist_fullRejects()
{
    Allowlist al;
    for (uint32_t i = 1; i <= Allowlist::MAX; i++)
        TEST_ASSERT_TRUE(al.add(i));
    TEST_ASSERT_FALSE(al.add(999));
    TEST_ASSERT_EQUAL_UINT(Allowlist::MAX, al.count());
}

static void test_allowlist_serializeRoundTripAndRejectsGarbage()
{
    Allowlist al;
    al.add(0x11);
    al.add(0x22);
    uint8_t buf[128];
    size_t n = al.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    Allowlist copy;
    TEST_ASSERT_TRUE(copy.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(2, copy.count());
    TEST_ASSERT_TRUE(copy.contains(0x22));

    buf[0] ^= 0xFF; // magic corrompido
    Allowlist bad;
    TEST_ASSERT_FALSE(bad.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(0, bad.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_station_windowLifecycleAndAnnounceCadence);
    RUN_TEST(test_station_grantOnlyInsideWindow);
    RUN_TEST(test_gateway_windowAndCooldown);
    RUN_TEST(test_allowlist_addRemoveContainsIdempotent);
    RUN_TEST(test_allowlist_fullRejects);
    RUN_TEST(test_allowlist_serializeRoundTripAndRejectsGarbage);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: RED** — Run suite `test_irrigation_pairing`. Expected: FAIL de compilação.

- [ ] **Step 3: Implementar**

`Pairing.h`:

```cpp
#pragma once
#include "modules/irrigation/IrrigationProtocol.h"
#include <stdint.h>

// Máquinas de estado do pareamento (spec §6). Puras: quem envia rádio é o módulo.
class StationPairing
{
  public:
    enum class State : uint8_t { IDLE, WINDOW, COMMITTED };
    static constexpr uint32_t WINDOW_MS = 2 * 60 * 1000, ANNOUNCE_INTERVAL_MS = 10 * 1000;

    void openWindow(uint32_t nowMs);
    void tick(uint32_t nowMs);
    bool announceDue(uint32_t nowMs);
    bool onGrant(const IrrigationProto::PairGrant &g, uint32_t nowMs);
    State state() const { return st; }
    const IrrigationProto::PairGrant &grant() const { return granted; }

  private:
    State st = State::IDLE;
    uint32_t windowStartMs = 0;
    uint32_t lastAnnounceMs = 0;
    bool announced = false;
    IrrigationProto::PairGrant granted = {};
};

class GatewayPairing
{
  public:
    static constexpr uint32_t WINDOW_MS = 2 * 60 * 1000, REGRANT_COOLDOWN_MS = 5 * 1000;

    void openWindow(uint32_t nowMs);
    void tick(uint32_t nowMs);
    bool windowOpen() const { return open; }
    bool approveAnnounce(uint32_t nodeId, uint32_t nowMs);

  private:
    bool open = false;
    uint32_t windowStartMs = 0;
    struct Recent {
        uint32_t node = 0;
        uint32_t atMs = 0;
    };
    Recent recent[4];
};
```

`Pairing.cpp`:

```cpp
#include "Pairing.h"

void StationPairing::openWindow(uint32_t nowMs)
{
    if (st == State::COMMITTED)
        return;
    st = State::WINDOW;
    windowStartMs = nowMs;
    announced = false;
}

void StationPairing::tick(uint32_t nowMs)
{
    if (st == State::WINDOW && (nowMs - windowStartMs) > WINDOW_MS)
        st = State::IDLE;
}

bool StationPairing::announceDue(uint32_t nowMs)
{
    tick(nowMs);
    if (st != State::WINDOW)
        return false;
    if (announced && (nowMs - lastAnnounceMs) < ANNOUNCE_INTERVAL_MS)
        return false;
    announced = true;
    lastAnnounceMs = nowMs;
    return true;
}

bool StationPairing::onGrant(const IrrigationProto::PairGrant &g, uint32_t nowMs)
{
    tick(nowMs);
    if (st != State::WINDOW)
        return false;
    granted = g;
    st = State::COMMITTED;
    return true;
}

void GatewayPairing::openWindow(uint32_t nowMs)
{
    open = true;
    windowStartMs = nowMs;
}

void GatewayPairing::tick(uint32_t nowMs)
{
    if (open && (nowMs - windowStartMs) > WINDOW_MS)
        open = false;
}

bool GatewayPairing::approveAnnounce(uint32_t nodeId, uint32_t nowMs)
{
    tick(nowMs);
    if (!open)
        return false;
    for (auto &r : recent) {
        if (r.node == nodeId && (nowMs - r.atMs) < REGRANT_COOLDOWN_MS)
            return false;
    }
    // registra (substitui a entrada mais antiga)
    Recent *slot = &recent[0];
    for (auto &r : recent)
        if (r.atMs < slot->atMs)
            slot = &r;
    slot->node = nodeId;
    slot->atMs = nowMs;
    return true;
}
```

`Allowlist.h`:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

// Allowlist de estações adotadas pelo gateway (spec §6). Persistência = módulo.
class Allowlist
{
  public:
    static constexpr size_t MAX = 16;
    static constexpr uint32_t MAGIC = 0x49414C31; // "IAL1"

    bool add(uint32_t nodeId);
    bool remove(uint32_t nodeId);
    bool contains(uint32_t nodeId) const;
    size_t count() const { return n; }
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t len);

  private:
    uint32_t ids[MAX] = {};
    size_t n = 0;
};
```

`Allowlist.cpp`:

```cpp
#include "Allowlist.h"
#include <string.h>

bool Allowlist::add(uint32_t nodeId)
{
    if (contains(nodeId))
        return true;
    if (n >= MAX)
        return false;
    ids[n++] = nodeId;
    return true;
}

bool Allowlist::remove(uint32_t nodeId)
{
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == nodeId) {
            ids[i] = ids[--n];
            return true;
        }
    }
    return false;
}

bool Allowlist::contains(uint32_t nodeId) const
{
    for (size_t i = 0; i < n; i++)
        if (ids[i] == nodeId)
            return true;
    return false;
}

size_t Allowlist::serialize(uint8_t *buf, size_t cap) const
{
    size_t need = 4 + 1 + 1 + n * 4;
    if (cap < need)
        return 0;
    memcpy(buf, &MAGIC, 4);
    buf[4] = 1; // versão
    buf[5] = (uint8_t)n;
    for (size_t i = 0; i < n; i++)
        memcpy(buf + 6 + i * 4, &ids[i], 4);
    return need;
}

bool Allowlist::deserialize(const uint8_t *buf, size_t len)
{
    n = 0;
    if (len < 6)
        return false;
    uint32_t magic;
    memcpy(&magic, buf, 4);
    if (magic != MAGIC || buf[4] != 1)
        return false;
    size_t cnt = buf[5];
    if (cnt > MAX || len != 6 + cnt * 4)
        return false;
    for (size_t i = 0; i < cnt; i++)
        memcpy(&ids[i], buf + 6 + i * 4, 4);
    n = cnt;
    return true;
}
```

Nota ao executor: `memcpy(buf, &MAGIC, 4)` sobre um `static constexpr` exige definição out-of-line em C++14, mas em C++17 `constexpr` membros são `inline` — ok. Se o linker reclamar, copie para uma local `uint32_t m = MAGIC;` antes do memcpy.

- [ ] **Step 4: GREEN** — Run suite `test_irrigation_pairing`. Expected: PASS (6 testes).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/Pairing.* src/modules/irrigation/Allowlist.* test/test_irrigation_pairing/
git commit -m "feat(irrigation): add pairing state machines and gateway allowlist"
```

---

### Task 5: Integração — thread de UI, handlers de pareamento, reset de fábrica, canal e reboot

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` / `.cpp`
- Modify: `test/native-suite-count` (36 → 38)
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (Fase 3 → concluída)

**Interfaces:**
- Consumes: tudo das Tasks 1–4; `Channels` global `channels` (`src/mesh/Channels.h`: `getByIndex`, `setChannel`, `onConfigChanged`, `getPrimary`, `getPrimaryIndex`, `getName`); `rebootAtMsec` (`extern uint32_t`, `src/main.h`); `nodeDB->getNodeNum()`; `owner.short_name` (nome p/ announce); padrão `saveIrrigationSettings` staged.
- Produces: firmware que pareia de ponta a ponta em bancada (2 nós + botões).

Decisões de projeto desta task (documentar em comentários no código):

1. **Aprovação física no gateway** (antecipação do painel da Fase 5): pressão CURTA no botão do gateway abre a janela de aceitação de 2 min. `PAIR_ANNOUNCE` dentro da janela → `PAIR_GRANT` automático + allowlist. Fora dela → só log.
2. **Canal**: as mensagens de pareamento trafegam no canal corrente do nó (nó de fábrica = canal default de flash). O `PAIR_GRANT` leva a PSK **primária** do gateway (`channels.getPrimary().psk` — se `psk.size != 32`, gateway recusa conceder e loga: fazenda ainda sem PSK própria de 32 bytes). A estação grava a PSK/nome no canal primário via `setChannel` + `onConfigChanged`, salva `boundGateway`, envia `EVENTO(EV_PAIRED)` e agenda `rebootAtMsec = millis() + 3000`.
3. **Reset de fábrica**: `HOLD_10S` → LED em PAIRING (confirmação visual §8.6) por 2 s, apaga `/prefs/irrigation.dat` e `/prefs/irrigation-allow.dat` (`FSCom.remove`), `EVENTO(EV_FACTORY_RESET)` e `rebootAtMsec = millis() + 2000`. (A PSK do canal permanece — remoção completa de credenciais de canal fica para a Fase 5 com o portal; documentar como limitação.)
4. **Thread de UI**: nova classe `IrrigationUiThread : concurrency::OSThread` dentro de `IrrigationModule.cpp` (25 ms; só instanciada se `pinBtn >= 0 || pinLed >= 0`), lê GPIO do botão (INPUT_PULLUP, pressionado = LOW), alimenta `ButtonGestureDetector`, chama `irrigationModule->onButtonEvent(ev)` e escreve `led.ledOn(millis())` no GPIO do LED. Em `ARCH_PORTDUINO` a leitura/escrita GPIO fica em stub (compila; sem hardware).
5. **Prioridade do LED** (função `refreshLedMode()` no módulo): PAIRING (janela estação OU gateway aberta) > BATTERY_SOS (vbat < limiar) > OUTPUT_OPEN (bitmap ≠ 0) > CONFIG_PENDING (safeMode) > NO_GATEWAY (sem heartbeat ACKado — nesta fase: `boundGateway != 0` e nunca recebeu nada dele; heurística simples, refina na Fase 4) > NORMAL.
6. **Gestos** (`onButtonEvent`): fábrica (`boundGateway == 0` e role ESTACAO): qualquer evento → `stationPairing.openWindow` + LED. Pareado ESTACAO: SHORT → `LOG_INFO` (portal Fase 5); DOUBLE → válvula 0: aberta? `close(0)` + `EVENTO(EV_MANUAL_CLOSE)` : `open(0, DEFAULT_MANUAL_OPEN_S=1200, maxOpenConfigS, millis())` + `EVENTO(EV_MANUAL_OPEN, 1200)`; LONG_3S → `open(0, 10, ...)` + `EVENTO(EV_TEST_PULSE)`; HOLD_10S → reset de fábrica. GATEWAY: SHORT → `gatewayPairing.openWindow`; HOLD_10S → reset.
7. **Handlers de rádio** em `handleReceived` (antes do switch atual, tipos novos): `MSG_PAIR_ANNOUNCE` (role GATEWAY: decode, `gatewayPairing.approveAnnounce(mp.from, millis())` → montar `PairGrant` e enviar direto ao nó + `allowlist.add` + persistir allowlist staged em `/prefs/irrigation-allow.dat`); `MSG_PAIR_GRANT` (role ESTACAO: decode, `stationPairing.onGrant` → commit: canal + settings + EVENTO + reboot). Pareamento NÃO passa por `senderAuthorized` (nó de fábrica não tem vínculo; janela + botão são a autorização), mas PASSA por anti-replay e rate limit.
8. **EVENTO**: helper `sendEvento(uint8_t code, uint32_t arg)` → destino `boundGateway` ou broadcast; sem ACK.
9. O envio periódico de `PAIR_ANNOUNCE` (10 s) sai do `runOnce()` (checar `stationPairing.announceDue(millis())` ANTES do early-return de role, só role ESTACAO na prática — colocar após o tick de válvulas com guarda de role ESTACAO).

Passos:

- [ ] **Step 1: Escrever o wiring** — mudanças em `IrrigationModule.h`:

```cpp
#include "modules/irrigation/Allowlist.h"
#include "modules/irrigation/ButtonGesture.h"
#include "modules/irrigation/LedPattern.h"
#include "modules/irrigation/Pairing.h"
// ... public:
    void onButtonEvent(ButtonGestureDetector::Event ev); // chamado pela IrrigationUiThread
    LedPatternController::Mode currentLedMode();
// ... private:
    void handlePairAnnounce(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handlePairGrant(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void commitPairing();
    void factoryReset();
    void logFarmKey();
    void sendEvento(uint8_t code, uint32_t arg = 0);
    void refreshLedMode();
    bool loadAllowlist();
    bool saveAllowlist();
    StationPairing stationPairing;
    GatewayPairing gatewayPairing;
    Allowlist allowlist;
    LedPatternController led;
    uint32_t lastGatewayRxMs = 0;
```

Corpo (em `IrrigationModule.cpp`) — handlers completos:

```cpp
void IrrigationModule::handlePairAnnounce(const meshtastic_MeshPacket &mp, const Header &h)
{
    if ((IrrigationRole)settings.role != IrrigationRole::GATEWAY)
        return;
    PairAnnounce pa;
    if (!decodePairAnnounce(mp.decoded.payload.bytes, mp.decoded.payload.size, pa))
        return;
    if (!gatewayPairing.approveAnnounce(mp.from, millis())) {
        LOG_INFO("Irrigation: announce from 0x%08x (%s) ignored, window closed", mp.from, pa.name);
        return;
    }
    const meshtastic_ChannelSettings &prim = channels.getPrimary();
    if (prim.psk.size != 32) {
        LOG_WARN("Irrigation: cannot grant, primary channel has no 32-byte PSK");
        return;
    }
    PairGrant g = {};
    memcpy(g.psk, prim.psk.bytes, 32);
    const char *chName = channels.getName(channels.getPrimaryIndex());
    g.nameLen = (uint8_t)strnlen(chName, 11);
    memcpy(g.channelName, chName, g.nameLen);
    g.gatewayId = nodeDB->getNodeNum();

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = mp.from;
    p->decoded.payload.size = (uint16_t)encodePairGrant(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, g);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    allowlist.add(mp.from);
    saveAllowlist();
    LOG_INFO("Irrigation: granted pairing to 0x%08x (%s)", mp.from, pa.name);
}

void IrrigationModule::handlePairGrant(const meshtastic_MeshPacket &mp, const Header &h)
{
    if ((IrrigationRole)settings.role != IrrigationRole::ESTACAO)
        return;
    PairGrant g;
    if (!decodePairGrant(mp.decoded.payload.bytes, mp.decoded.payload.size, g))
        return;
    if (!stationPairing.onGrant(g, millis())) {
        LOG_WARN("Irrigation: grant from 0x%08x outside pairing window", mp.from);
        return;
    }
    commitPairing();
}

void IrrigationModule::commitPairing()
{
    const PairGrant &g = stationPairing.grant();
    // Canal primário recebe a PSK/nome da fazenda (spec §6 passo 4)
    meshtastic_Channel ch = channels.getByIndex(channels.getPrimaryIndex());
    memcpy(ch.settings.psk.bytes, g.psk, 32);
    ch.settings.psk.size = 32;
    memset(ch.settings.name, 0, sizeof(ch.settings.name));
    memcpy(ch.settings.name, g.channelName, g.nameLen);
    channels.setChannel(ch);
    channels.onConfigChanged();

    settings.boundGateway = g.gatewayId;
    if (!saveIrrigationSettings(settings)) {
        LOG_ERROR("Irrigation: pairing commit failed to persist");
        return; // sem persistir vínculo não reinicia; próxima janela tenta de novo
    }
    safeMode = false;
    sendEvento(EV_PAIRED, g.gatewayId);
    LOG_INFO("Irrigation: paired to gateway 0x%08x, rebooting", g.gatewayId);
    rebootAtMsec = millis() + 3000;
}

void IrrigationModule::logFarmKey()
{
    // Pressão longa no botão do gateway despeja a chave da fazenda no console
    // serial em base64 (nome do canal + PSK), para cadastro manual no cofre do
    // device de serviço (spec §11.2). Acesso físico ao gateway = autorização —
    // mesmo modelo do pareamento. O painel da Fase 5 ganha a mesma função na UI.
    const meshtastic_ChannelSettings &prim = channels.getPrimary();
    if (prim.psk.size != 32) {
        LOG_WARN("Irrigation: primary channel has no 32-byte PSK to export");
        return;
    }
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[45];
    int o = 0;
    for (int i = 0; i < 32; i += 3) {
        uint32_t v = ((uint32_t)prim.psk.bytes[i] << 16) | ((i + 1 < 32 ? prim.psk.bytes[i + 1] : 0) << 8) |
                     (i + 2 < 32 ? prim.psk.bytes[i + 2] : 0);
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = (i + 1 < 32) ? b64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < 32) ? b64[v & 63] : '=';
    }
    out[o] = '\0';
    LOG_INFO("Irrigation FARM KEY: channel=%s psk_b64=%s", channels.getName(channels.getPrimaryIndex()), out);
}

void IrrigationModule::factoryReset()
{
    LOG_WARN("Irrigation: factory reset by button");
    sendEvento(EV_FACTORY_RESET);
#ifdef FSCom
    FSCom.remove("/prefs/irrigation.dat");
    FSCom.remove("/prefs/irrigation-allow.dat");
#endif
    rebootAtMsec = millis() + 2000;
}

void IrrigationModule::sendEvento(uint8_t code, uint32_t arg)
{
    Evento ev = {code, arg};
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = settings.boundGateway ? settings.boundGateway : NODENUM_BROADCAST;
    p->decoded.payload.size = (uint16_t)encodeEvento(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, ev);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
}

void IrrigationModule::onButtonEvent(ButtonGestureDetector::Event ev)
{
    using Ev = ButtonGestureDetector::Event;
    IrrigationRole role = (IrrigationRole)settings.role;
    if (ev == Ev::HOLD_10S) { // reset vale em qualquer papel/estado
        factoryReset();
        return;
    }
    if (role == IrrigationRole::ESTACAO && settings.boundGateway == 0) {
        // Nó de fábrica: qualquer pressão abre a janela de pareamento (§8.6)
        stationPairing.openWindow(millis());
        LOG_INFO("Irrigation: pairing window open (2 min)");
        return;
    }
    if (role == IrrigationRole::GATEWAY) {
        if (ev == Ev::SHORT) {
            gatewayPairing.openWindow(millis());
            LOG_INFO("Irrigation: gateway accept window open (2 min)");
        } else if (ev == Ev::LONG_3S) {
            logFarmKey(); // requisito do usuário: exportar PSK p/ o device de serviço (§11.2)
        }
        return;
    }
    if (role != IrrigationRole::ESTACAO)
        return;
    switch (ev) {
    case Ev::SHORT:
        LOG_INFO("Irrigation: portal request (fase 5)");
        break;
    case Ev::DOUBLE:
        if (valves.isOpen(0)) {
            valves.close(0);
            sendEvento(EV_MANUAL_CLOSE);
        } else if (valves.open(0, DEFAULT_MANUAL_OPEN_S, settings.maxOpenConfigS, millis()) == ValveController::Result::OK) {
            sendEvento(EV_MANUAL_OPEN, DEFAULT_MANUAL_OPEN_S);
        }
        break;
    case Ev::LONG_3S:
        if (valves.open(0, 10, settings.maxOpenConfigS, millis()) == ValveController::Result::OK)
            sendEvento(EV_TEST_PULSE);
        break;
    default:
        break;
    }
}
```

Com `constexpr uint32_t DEFAULT_MANUAL_OPEN_S = 20 * 60;` no topo do .cpp (padrao_min do exemplo §5.2). `refreshLedMode()`:

```cpp
void IrrigationModule::refreshLedMode()
{
    using Mode = LedPatternController::Mode;
    uint16_t vbat = batteryCentiV();
    if (stationPairing.state() == StationPairing::State::WINDOW || gatewayPairing.windowOpen())
        led.setMode(Mode::PAIRING);
    else if (vbat != 0 && vbat < settings.vbatMinAbrirCentiV)
        led.setMode(Mode::BATTERY_SOS);
    else if (valves.stateBitmap() != 0)
        led.setMode(Mode::OUTPUT_OPEN);
    else if (safeMode)
        led.setMode(Mode::CONFIG_PENDING);
    else if (settings.boundGateway != 0 && lastGatewayRxMs == 0)
        led.setMode(Mode::NO_GATEWAY);
    else
        led.setMode(Mode::NORMAL);
}
```

(`lastGatewayRxMs = millis()` em qualquer pacote com `mp.from == settings.boundGateway` no `handleReceived`.) `refreshLedMode()` chamado no fim de `runOnce()`; announce periódico:

```cpp
    // no runOnce, após o tick de válvulas (role ESTACAO):
    if (stationPairing.announceDue(millis())) {
        PairAnnounce pa = {};
        pa.protoVersion = VERSION;
        pa.nameLen = (uint8_t)strnlen(owner.short_name, 15);
        memcpy(pa.name, owner.short_name, pa.nameLen);
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = NODENUM_BROADCAST;
        p->decoded.payload.size = (uint16_t)encodePairAnnounce(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, pa);
        if (p->decoded.payload.size)
            service->sendToMesh(p, RX_SRC_LOCAL, false);
        else
            packetPool.release(p);
    }
```

`IrrigationUiThread` (no fim de `IrrigationModule.cpp`, antes do construtor usar):

```cpp
class IrrigationUiThread : public concurrency::OSThread
{
  public:
    IrrigationUiThread(IrrigationModule *m, int8_t btnPin, int8_t ledPin)
        : OSThread("IrrigUi"), module(m), btn(btnPin), ledPin(ledPin)
    {
#ifndef ARCH_PORTDUINO
        if (btn >= 0)
            pinMode(btn, INPUT_PULLUP);
        if (ledPin >= 0)
            pinMode(ledPin, OUTPUT);
#endif
    }

  protected:
    int32_t runOnce() override
    {
        bool pressed = false;
#ifndef ARCH_PORTDUINO
        if (btn >= 0)
            pressed = digitalRead(btn) == LOW;
#endif
        auto ev = detector.update(pressed, millis());
        if (ev != ButtonGestureDetector::Event::NONE)
            module->onButtonEvent(ev);
#ifndef ARCH_PORTDUINO
        if (ledPin >= 0)
            digitalWrite(ledPin, module->ledOnNow() ? HIGH : LOW);
#endif
        return 25;
    }

  private:
    IrrigationModule *module;
    int8_t btn, ledPin;
    ButtonGestureDetector detector;
};
```

com `bool IrrigationModule::ledOnNow() { return led.ledOn(millis()); }` público, e no construtor do módulo: `if (settings.pinBtn >= 0 || settings.pinLed >= 0) new IrrigationUiThread(this, settings.pinBtn, settings.pinLed);`. Allowlist load/save com o mesmo padrão staged do settings (arquivo `/prefs/irrigation-allow.dat`, buffer de `6 + 16*4 = 70` bytes via `serialize`/`deserialize`); `loadAllowlist()` no construtor quando role GATEWAY. Includes novos no .cpp: `"NodeDB.h"` (owner/nodeDB), `"mesh/Channels.h"` já via outros headers — verificar.

Switch do `handleReceived` ganha:

```cpp
    case MSG_PAIR_ANNOUNCE:
        handlePairAnnounce(mp, h);
        break;
    case MSG_PAIR_GRANT:
        handlePairGrant(mp, h);
        break;
```

(ambos APÓS anti-replay/rate-limit genéricos? Não — o fluxo atual faz essas checagens dentro de cada handler de comando. Para pareamento: aplicar `seqTable.checkAndUpdate(mp.from, h.seq)` e `rateLimiter.allow(millis())` no INÍCIO de cada handler novo, SEM `senderAuthorized`.)

- [ ] **Step 2: Compilar via suites** — Run suites `test_irrigation_valve` e `test_irrigation_pairing` (compilam src inteiro). Expected: PASSED, sem erros citando IrrigationModule/UiThread.

- [ ] **Step 3: `test/native-suite-count` 36 → 38 e roadmap** — Fase 3 na tabela → `concluída (data, plano 2026-07-12-irrigacao-fase3-pareamento.md; aprovação física no gateway antecipa o painel da Fase 5; PSK de canal não é apagada no reset de fábrica — limitação até a Fase 5)`.

- [ ] **Step 4: Suite completa** — comando full (timeout -k 60 3000, `tail -8`). Expected: 38 suites, 0 failed (625 + 19 novos = 644 test cases; o número exato pode variar com os ajustes da Task 2 — validar `succeeded == total`).

- [ ] **Step 5: Commits**

```bash
git add src/modules/irrigation/IrrigationModule.* 
git commit -m "feat(irrigation): pairing flow, factory reset, button gestures and status led"
git add test/native-suite-count docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "test(irrigation): register phase 3 suites and update roadmap"
```

---

## Self-Review (executado na escrita do plano)

- **Cobertura**: §6 fluxo de adoção passos 1–4 ✔ (aprovação física substitui painel — documentado como antecipação); §6 reset de fábrica ✔ (limitação: PSK de canal permanece — registrada); repetidores sem allowlist ✔ (só GATEWAY concede/liste); §8.6 tabela de gestos ✔ (curta em nó de fábrica = janela, spec diz "qualquer pressão" ✔; portal = Fase 5 stub); §8.7 tabela única ✔ com prioridade documentada; EVENTO para ações manuais ✔ (consumo na Fase 4/6). WPA2/PIN do portal = Fase 5.
- **Placeholders**: nenhum; notas ao executor apontam verificações (ordem LONG/HOLD, linkage do MAGIC, includes) — não código faltante.
- **Consistência**: `ButtonGestureDetector::Event`/`update(bool,uint32_t)` idênticos Tasks 3/5; `StationPairing::{openWindow,tick,announceDue,onGrant,state,grant}` e `GatewayPairing::{openWindow,tick,windowOpen,approveAnnounce}` idênticos Tasks 4/5; `Allowlist::{add,remove,contains,count,serialize,deserialize}` idem; `PairGrant`/`PairAnnounce`/`Evento` e `EV_*` das Tasks 1/5 batem; `DEFAULT_MANUAL_OPEN_S=1200` só na Task 5 (uso único).
