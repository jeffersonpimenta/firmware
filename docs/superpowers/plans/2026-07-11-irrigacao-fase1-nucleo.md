# Irrigação Mesh — Fase 1: Núcleo do protocolo + estação fail-safe

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Módulo Meshtastic custom (`IrrigationModule`) no portnum privado que recebe `CMD_VALVULA`, aciona válvulas latching com timer fail-safe local, responde `ACK`, envia `HEARTBEAT` periódico — com anti-replay, rate limit e bloqueio por bateria baixa. Tudo com testes nativos.

**Architecture:** Lógica pura (protocolo binário, controlador de válvulas, anti-replay, rate limit) em classes sem dependência de hardware, testáveis com Unity nativo. Camada fina de integração (`IrrigationModule` herdando `SinglePortModule` + `OSThread`) faz o glue com o rádio, GPIO e settings persistidos em arquivo. Papéis (role) definidos em settings; gateway/painel ficam para fases seguintes.

**Tech Stack:** C++17, Meshtastic module framework (`SinglePortModule`, `OSThread`), Unity (testes nativos via `./bin/run-tests.sh`), FSCommon para persistência de settings.

## Global Constraints

- Teto absoluto de abertura: **120 min**, compilado no firmware (`MAX_OPEN_SECONDS = 120*60`). Valor efetivo = `min(comando, config_local, teto)` (spec §4.2).
- Comando de abrir com saída já ativa **não é erro**: reinicia o timer fail-safe (renovação, §4.2).
- Bateria abaixo do limiar: **recusa abrir** (NACK com motivo), **sempre aceita fechar** (§4.2).
- Rate limit padrão: **10 comandos/min** no receptor (§10).
- Payload ≤ **200 bytes** (§3.2). Portnum: `meshtastic_PortNum_PRIVATE_APP` (256).
- Toda mensagem carrega **versão de protocolo**; mismatch = rejeição segura + log (§3.1).
- Anti-replay: `last_seq` **por remetente**; seq já vista/antiga é descartada e logada (§4.2). (Persistência do contador em NVS = Fase 2; nesta fase é RAM.)
- Vínculo com gateway: se `boundGateway != 0`, só ele comanda. `boundGateway == 0` (não pareado) aceita qualquer remetente **nesta fase** — o pareamento da Fase 3 fecha isso; a decisão fica registrada no código com comentário citando a fase.
- Estilo do repo: `LOG_DEBUG/INFO/WARN/ERROR`; node IDs em log como `0x%08x`; `trunk fmt` antes de commit; comentários mínimos.
- Nunca editar `src/mesh/generated/`.
- Testes: cada suite nova em `test/test_<nome>/test_main.cpp` no padrão Unity do repo (`setup()` chama `initializeTestEnvironment()`, `RUN_TEST`, `exit(UNITY_END())`, `void loop() {}`); **atualizar `test/native-suite-count`** (hoje `31`).
- Rodar testes: `./bin/run-tests.sh` (exit 0 GREEN). Suite única: `pio test -e native --filter test_irrigation_protocol` (ou via docker `./bin/test-native-docker.sh` se host sem deps Linux).

## File Structure

```
src/modules/irrigation/
  IrrigationProtocol.h/.cpp   # tipos de mensagem, encode/decode binário LE (Task 1)
  SeqTable.h/.cpp             # anti-replay last_seq por remetente (Task 2)
  RateLimiter.h/.cpp          # janela de 60 s, N cmds/min (Task 2)
  ValveController.h/.cpp      # timer fail-safe local, renovação, lockout de bateria (Task 3)
  IrrigationSettings.h/.cpp   # role, pinos, limiares; load/save em /prefs (Task 4)
  IrrigationModule.h/.cpp     # glue: SinglePortModule + OSThread + GPIO driver (Task 5)
test/test_irrigation_protocol/test_main.cpp   (Task 1)
test/test_irrigation_replay/test_main.cpp      (Task 2)
test/test_irrigation_valve/test_main.cpp       (Task 3)
src/modules/Modules.cpp        # registro (Task 5)
test/native-suite-count        # 31 → 34 (Task 5, após as 3 suites existirem)
```

---

### Task 1: Protocolo binário (`IrrigationProtocol`)

**Files:**
- Create: `src/modules/irrigation/IrrigationProtocol.h`
- Create: `src/modules/irrigation/IrrigationProtocol.cpp`
- Test: `test/test_irrigation_protocol/test_main.cpp`

**Interfaces:**
- Consumes: nada (lógica pura).
- Produces: namespace `IrrigationProto` — structs `Header`, `CmdValvula`, `CmdGpo`, `Ack`, `Heartbeat`; enums `MsgType` (`MSG_CMD_VALVULA=1 … MSG_RESYNC_SEQ=11`), `NackReason`, `AckStatus`, `HbFlags`; funções `encodeCmdValvula/encodeCmdGpo/encodeAck/encodeHeartbeat(uint8_t *buf, size_t len, uint32_t seq, const T &m) → size_t` (0 = buffer pequeno) e `decodeHeader/decodeCmdValvula/decodeCmdGpo/decodeAck/decodeHeartbeat(const uint8_t *buf, size_t len, T &out) → bool`. Constantes `VERSION=1`, `HEADER_LEN=8`, `MAX_PAYLOAD=200`, `MAX_OPEN_SECONDS=7200`.

- [ ] **Step 1: Escrever teste que falha**

`test/test_irrigation_protocol/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include <string.h>
#include <unity.h>

using namespace IrrigationProto;

void setUp(void) {}
void tearDown(void) {}

static void test_cmdValvula_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    CmdValvula in = {2, 1, 1800};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 42, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(VERSION, h.version);
    TEST_ASSERT_EQUAL_UINT8(MSG_CMD_VALVULA, h.type);
    TEST_ASSERT_EQUAL_UINT32(42, h.seq);

    CmdValvula out;
    TEST_ASSERT_TRUE(decodeCmdValvula(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(2, out.valveId);
    TEST_ASSERT_EQUAL_UINT8(1, out.action);
    TEST_ASSERT_EQUAL_UINT16(1800, out.durationS);
}

static void test_ack_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    Ack in = {42, ACK_NACK, REASON_BATTERY_LOW, 0b0101, 0, 1215, 17};
    size_t n = encodeAck(buf, sizeof(buf), 7, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Ack out;
    TEST_ASSERT_TRUE(decodeAck(buf, n, out));
    TEST_ASSERT_EQUAL_UINT32(42, out.ackedSeq);
    TEST_ASSERT_EQUAL_UINT8(ACK_NACK, out.status);
    TEST_ASSERT_EQUAL_UINT8(REASON_BATTERY_LOW, out.reason);
    TEST_ASSERT_EQUAL_UINT8(0b0101, out.valveStates);
    TEST_ASSERT_EQUAL_UINT16(1215, out.vbatCentiV);
    TEST_ASSERT_EQUAL_UINT32(17, out.configEpoch);
}

static void test_heartbeat_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    Heartbeat in = {};
    in.valveStates = 0b10;
    in.vbatCentiV = 1250;
    in.vpanelCentiV = 1810;
    in.rssi = -97;
    in.snrQuarterDb = -22; // -5,5 dB em quartos de dB
    in.rebootCount = 3;
    in.rebootCause = 2;
    in.flags = HB_FLAG_TAMPER;
    in.configEpoch = 9;
    size_t n = encodeHeartbeat(buf, sizeof(buf), 100, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Heartbeat out;
    TEST_ASSERT_TRUE(decodeHeartbeat(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(0b10, out.valveStates);
    TEST_ASSERT_EQUAL_UINT16(1250, out.vbatCentiV);
    TEST_ASSERT_EQUAL_INT16(-97, out.rssi);
    TEST_ASSERT_EQUAL_INT8(-22, out.snrQuarterDb);
    TEST_ASSERT_EQUAL_UINT16(3, out.rebootCount);
    TEST_ASSERT_EQUAL_UINT8(HB_FLAG_TAMPER, out.flags);
    TEST_ASSERT_EQUAL_UINT32(9, out.configEpoch);
}

static void test_truncatedBuffer_rejected()
{
    uint8_t buf[MAX_PAYLOAD];
    CmdValvula in = {0, 1, 60};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 1, in);

    Header h;
    TEST_ASSERT_FALSE(decodeHeader(buf, HEADER_LEN - 1, h));
    CmdValvula out;
    TEST_ASSERT_FALSE(decodeCmdValvula(buf, n - 1, out));
}

static void test_encodeBufferTooSmall_returnsZero()
{
    uint8_t buf[4];
    CmdValvula in = {0, 1, 60};
    TEST_ASSERT_EQUAL_UINT(0, encodeCmdValvula(buf, sizeof(buf), 1, in));
}

static void test_cmdGpo_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    CmdGpo in = {1, 1, 0}; // duração 0 = biestável
    size_t n = encodeCmdGpo(buf, sizeof(buf), 5, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    CmdGpo out;
    TEST_ASSERT_TRUE(decodeCmdGpo(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(1, out.gpoId);
    TEST_ASSERT_EQUAL_UINT16(0, out.durationS);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_cmdValvula_roundTrip);
    RUN_TEST(test_ack_roundTrip);
    RUN_TEST(test_heartbeat_roundTrip);
    RUN_TEST(test_truncatedBuffer_rejected);
    RUN_TEST(test_encodeBufferTooSmall_returnsZero);
    RUN_TEST(test_cmdGpo_roundTrip);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `pio test -e native --filter test_irrigation_protocol`
Expected: FAIL na compilação — `IrrigationProtocol.h: No such file or directory`.

- [ ] **Step 3: Implementar**

`src/modules/irrigation/IrrigationProtocol.h`:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace IrrigationProto
{

constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_LEN = 8;
constexpr size_t MAX_PAYLOAD = 200;
constexpr uint32_t MAX_OPEN_SECONDS = 120 * 60; // teto absoluto compilado (spec §4.2)

enum MsgType : uint8_t {
    MSG_CMD_VALVULA = 1,
    MSG_CMD_GPO = 2,
    MSG_ACK = 3,
    MSG_HEARTBEAT = 4,
    MSG_SET_CONFIG = 5,
    MSG_GET_CONFIG = 6,
    MSG_EVENTO = 7,
    MSG_PAIR_ANNOUNCE = 8,
    MSG_PAIR_GRANT = 9,
    MSG_PING_SURVEY = 10,
    MSG_RESYNC_SEQ = 11,
};

enum AckStatus : uint8_t { ACK_OK = 0, ACK_NACK = 1 };

enum NackReason : uint8_t {
    REASON_NONE = 0,
    REASON_BATTERY_LOW = 1,
    REASON_RATE_LIMIT = 2,
    REASON_INVALID_ID = 3,
    REASON_UNAUTHORIZED = 4,
    REASON_BAD_VERSION = 5,
    REASON_BAD_PAYLOAD = 6,
};

enum HbFlags : uint8_t {
    HB_FLAG_TAMPER = 1 << 0,
    HB_FLAG_SAFE_MODE = 1 << 1,
    HB_FLAG_HIBERNATION = 1 << 2,
};

struct Header {
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t seq;
};

struct CmdValvula {
    uint8_t valveId;
    uint8_t action; // 0 = fechar, 1 = abrir
    uint16_t durationS;
};

struct CmdGpo {
    uint8_t gpoId;
    uint8_t action;
    uint16_t durationS; // 0 = biestável
};

struct Ack {
    uint32_t ackedSeq;
    uint8_t status; // AckStatus
    uint8_t reason; // NackReason
    uint8_t valveStates;
    uint8_t gpoStates;
    uint16_t vbatCentiV;
    uint32_t configEpoch;
};

struct Heartbeat {
    uint8_t valveStates;
    uint8_t gpoStates;
    uint16_t vbatCentiV;
    uint16_t vpanelCentiV;
    int16_t rssi;
    int8_t snrQuarterDb;
    uint16_t rebootCount;
    uint8_t rebootCause;
    uint8_t flags; // HbFlags
    uint32_t configEpoch;
};

// Encoders devolvem bytes totais gravados (header + corpo), 0 se buffer pequeno.
size_t encodeCmdValvula(uint8_t *buf, size_t len, uint32_t seq, const CmdValvula &m);
size_t encodeCmdGpo(uint8_t *buf, size_t len, uint32_t seq, const CmdGpo &m);
size_t encodeAck(uint8_t *buf, size_t len, uint32_t seq, const Ack &m);
size_t encodeHeartbeat(uint8_t *buf, size_t len, uint32_t seq, const Heartbeat &m);

// decodeHeader primeiro; depois o decode do corpo conforme header.type.
bool decodeHeader(const uint8_t *buf, size_t len, Header &out);
bool decodeCmdValvula(const uint8_t *buf, size_t len, CmdValvula &out);
bool decodeCmdGpo(const uint8_t *buf, size_t len, CmdGpo &out);
bool decodeAck(const uint8_t *buf, size_t len, Ack &out);
bool decodeHeartbeat(const uint8_t *buf, size_t len, Heartbeat &out);

} // namespace IrrigationProto
```

`src/modules/irrigation/IrrigationProtocol.cpp`:

```cpp
#include "IrrigationProtocol.h"

namespace IrrigationProto
{

namespace
{
// Serialização little-endian explícita: independe de endianness/padding do alvo.
struct Writer {
    uint8_t *buf;
    size_t cap;
    size_t pos = 0;
    bool ok = true;
    void u8(uint8_t v)
    {
        if (pos + 1 > cap) {
            ok = false;
            return;
        }
        buf[pos++] = v;
    }
    void u16(uint16_t v)
    {
        u8(v & 0xff);
        u8(v >> 8);
    }
    void u32(uint32_t v)
    {
        u16(v & 0xffff);
        u16(v >> 16);
    }
    void i8(int8_t v) { u8((uint8_t)v); }
    void i16(int16_t v) { u16((uint16_t)v); }
};

struct Reader {
    const uint8_t *buf;
    size_t len;
    size_t pos = 0;
    bool ok = true;
    uint8_t u8()
    {
        if (pos + 1 > len) {
            ok = false;
            return 0;
        }
        return buf[pos++];
    }
    uint16_t u16()
    {
        uint16_t lo = u8();
        return lo | ((uint16_t)u8() << 8);
    }
    uint32_t u32()
    {
        uint32_t lo = u16();
        return lo | ((uint32_t)u16() << 16);
    }
    int8_t i8() { return (int8_t)u8(); }
    int16_t i16() { return (int16_t)u16(); }
};

void writeHeader(Writer &w, uint8_t type, uint32_t seq)
{
    w.u8(VERSION);
    w.u8(type);
    w.u16(0); // flags (reservado; SERVICE_MAGIC na fase 8)
    w.u32(seq);
}

Reader bodyReader(const uint8_t *buf, size_t len)
{
    Reader r{buf, len};
    r.pos = HEADER_LEN;
    if (len < HEADER_LEN)
        r.ok = false;
    return r;
}
} // namespace

size_t encodeCmdValvula(uint8_t *buf, size_t len, uint32_t seq, const CmdValvula &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_CMD_VALVULA, seq);
    w.u8(m.valveId);
    w.u8(m.action);
    w.u16(m.durationS);
    return w.ok ? w.pos : 0;
}

size_t encodeCmdGpo(uint8_t *buf, size_t len, uint32_t seq, const CmdGpo &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_CMD_GPO, seq);
    w.u8(m.gpoId);
    w.u8(m.action);
    w.u16(m.durationS);
    return w.ok ? w.pos : 0;
}

size_t encodeAck(uint8_t *buf, size_t len, uint32_t seq, const Ack &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_ACK, seq);
    w.u32(m.ackedSeq);
    w.u8(m.status);
    w.u8(m.reason);
    w.u8(m.valveStates);
    w.u8(m.gpoStates);
    w.u16(m.vbatCentiV);
    w.u32(m.configEpoch);
    return w.ok ? w.pos : 0;
}

size_t encodeHeartbeat(uint8_t *buf, size_t len, uint32_t seq, const Heartbeat &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_HEARTBEAT, seq);
    w.u8(m.valveStates);
    w.u8(m.gpoStates);
    w.u16(m.vbatCentiV);
    w.u16(m.vpanelCentiV);
    w.i16(m.rssi);
    w.i8(m.snrQuarterDb);
    w.u16(m.rebootCount);
    w.u8(m.rebootCause);
    w.u8(m.flags);
    w.u32(m.configEpoch);
    return w.ok ? w.pos : 0;
}

bool decodeHeader(const uint8_t *buf, size_t len, Header &out)
{
    Reader r{buf, len};
    out.version = r.u8();
    out.type = r.u8();
    out.flags = r.u16();
    out.seq = r.u32();
    return r.ok;
}

bool decodeCmdValvula(const uint8_t *buf, size_t len, CmdValvula &out)
{
    Reader r = bodyReader(buf, len);
    out.valveId = r.u8();
    out.action = r.u8();
    out.durationS = r.u16();
    return r.ok;
}

bool decodeCmdGpo(const uint8_t *buf, size_t len, CmdGpo &out)
{
    Reader r = bodyReader(buf, len);
    out.gpoId = r.u8();
    out.action = r.u8();
    out.durationS = r.u16();
    return r.ok;
}

bool decodeAck(const uint8_t *buf, size_t len, Ack &out)
{
    Reader r = bodyReader(buf, len);
    out.ackedSeq = r.u32();
    out.status = r.u8();
    out.reason = r.u8();
    out.valveStates = r.u8();
    out.gpoStates = r.u8();
    out.vbatCentiV = r.u16();
    out.configEpoch = r.u32();
    return r.ok;
}

bool decodeHeartbeat(const uint8_t *buf, size_t len, Heartbeat &out)
{
    Reader r = bodyReader(buf, len);
    out.valveStates = r.u8();
    out.gpoStates = r.u8();
    out.vbatCentiV = r.u16();
    out.vpanelCentiV = r.u16();
    out.rssi = r.i16();
    out.snrQuarterDb = r.i8();
    out.rebootCount = r.u16();
    out.rebootCause = r.u8();
    out.flags = r.u8();
    out.configEpoch = r.u32();
    return r.ok;
}

} // namespace IrrigationProto
```

- [ ] **Step 4: Rodar e ver passar**

Run: `pio test -e native --filter test_irrigation_protocol`
Expected: PASS (6 testes).

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt
git add src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationProtocol.cpp test/test_irrigation_protocol/
git commit -m "feat(irrigation): add binary application protocol codec"
```

---

### Task 2: Anti-replay (`SeqTable`) + rate limit (`RateLimiter`)

**Files:**
- Create: `src/modules/irrigation/SeqTable.h`
- Create: `src/modules/irrigation/SeqTable.cpp`
- Create: `src/modules/irrigation/RateLimiter.h`
- Create: `src/modules/irrigation/RateLimiter.cpp`
- Test: `test/test_irrigation_replay/test_main.cpp`

**Interfaces:**
- Consumes: nada.
- Produces: `class SeqTable { bool checkAndUpdate(uint32_t sender, uint32_t seq); uint32_t lastSeq(uint32_t sender) const; }` (true = seq fresca, aceita e registrada); `class RateLimiter { explicit RateLimiter(uint8_t maxPerMinute); bool allow(uint32_t nowMs); }`.

- [ ] **Step 1: Escrever teste que falha**

`test/test_irrigation_replay/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/RateLimiter.h"
#include "modules/irrigation/SeqTable.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_firstSeqFromSender_accepted()
{
    SeqTable t;
    TEST_ASSERT_TRUE(t.checkAndUpdate(0xa1b2c3d4, 100));
    TEST_ASSERT_EQUAL_UINT32(100, t.lastSeq(0xa1b2c3d4));
}

static void test_replayAndOldSeq_rejected()
{
    SeqTable t;
    t.checkAndUpdate(0xa1b2c3d4, 100);
    TEST_ASSERT_FALSE(t.checkAndUpdate(0xa1b2c3d4, 100)); // replay
    TEST_ASSERT_FALSE(t.checkAndUpdate(0xa1b2c3d4, 99));  // antiga
    TEST_ASSERT_TRUE(t.checkAndUpdate(0xa1b2c3d4, 101));
}

static void test_sendersIndependent()
{
    SeqTable t;
    TEST_ASSERT_TRUE(t.checkAndUpdate(0x11111111, 50));
    TEST_ASSERT_TRUE(t.checkAndUpdate(0x22222222, 50)); // mesmo seq, remetente diferente
}

static void test_tableFull_overwritesLowestSeq()
{
    SeqTable t;
    for (uint32_t i = 0; i < SeqTable::MAX_PEERS; i++)
        TEST_ASSERT_TRUE(t.checkAndUpdate(0x1000 + i, 10 + i));
    // Tabela cheia: novo remetente desaloja a entrada de menor seq (0x1000)
    TEST_ASSERT_TRUE(t.checkAndUpdate(0x9999, 5));
    TEST_ASSERT_EQUAL_UINT32(5, t.lastSeq(0x9999));
    TEST_ASSERT_EQUAL_UINT32(0, t.lastSeq(0x1000)); // esquecido
}

static void test_rateLimiter_blocksAboveLimit()
{
    RateLimiter rl(3);
    TEST_ASSERT_TRUE(rl.allow(1000));
    TEST_ASSERT_TRUE(rl.allow(2000));
    TEST_ASSERT_TRUE(rl.allow(3000));
    TEST_ASSERT_FALSE(rl.allow(4000));
}

static void test_rateLimiter_windowResets()
{
    RateLimiter rl(2);
    TEST_ASSERT_TRUE(rl.allow(1000));
    TEST_ASSERT_TRUE(rl.allow(2000));
    TEST_ASSERT_FALSE(rl.allow(3000));
    TEST_ASSERT_TRUE(rl.allow(1000 + 60000)); // nova janela de 60 s
}

static void test_rateLimiter_millisRollover()
{
    RateLimiter rl(1);
    TEST_ASSERT_TRUE(rl.allow(0xFFFFF000u));
    TEST_ASSERT_FALSE(rl.allow(0xFFFFFF00u));
    TEST_ASSERT_TRUE(rl.allow(0xFFFFF000u + 60000)); // atravessa o wrap
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_firstSeqFromSender_accepted);
    RUN_TEST(test_replayAndOldSeq_rejected);
    RUN_TEST(test_sendersIndependent);
    RUN_TEST(test_tableFull_overwritesLowestSeq);
    RUN_TEST(test_rateLimiter_blocksAboveLimit);
    RUN_TEST(test_rateLimiter_windowResets);
    RUN_TEST(test_rateLimiter_millisRollover);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `pio test -e native --filter test_irrigation_replay`
Expected: FAIL na compilação — headers inexistentes.

- [ ] **Step 3: Implementar**

`src/modules/irrigation/SeqTable.h`:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

// Anti-replay (spec §4.2): last_seq por remetente. Tabela fixa em RAM;
// persistência em NVS entra na Fase 2 do plano.
class SeqTable
{
  public:
    static constexpr size_t MAX_PEERS = 8;

    // true = seq nova (aceita e registrada); false = replay/antiga (descartar).
    bool checkAndUpdate(uint32_t sender, uint32_t seq);
    // 0 se o remetente é desconhecido.
    uint32_t lastSeq(uint32_t sender) const;

  private:
    struct Entry {
        uint32_t node = 0;
        uint32_t seq = 0;
    };
    Entry entries[MAX_PEERS];
};
```

`src/modules/irrigation/SeqTable.cpp`:

```cpp
#include "SeqTable.h"

bool SeqTable::checkAndUpdate(uint32_t sender, uint32_t seq)
{
    Entry *empty = nullptr;
    Entry *lowest = &entries[0];
    for (auto &e : entries) {
        if (e.node == sender) {
            if (seq <= e.seq)
                return false;
            e.seq = seq;
            return true;
        }
        if (e.node == 0 && !empty)
            empty = &e;
        if (e.seq < lowest->seq)
            lowest = &e;
    }
    Entry *slot = empty ? empty : lowest;
    slot->node = sender;
    slot->seq = seq;
    return true;
}

uint32_t SeqTable::lastSeq(uint32_t sender) const
{
    for (const auto &e : entries)
        if (e.node == sender)
            return e.seq;
    return 0;
}
```

`src/modules/irrigation/RateLimiter.h`:

```cpp
#pragma once
#include <stdint.h>

// Rate limit de comandos no receptor (spec §4.2). Janela fixa de 60 s.
class RateLimiter
{
  public:
    explicit RateLimiter(uint8_t maxPerMinute) : maxPerMinute(maxPerMinute) {}
    bool allow(uint32_t nowMs);

  private:
    uint8_t maxPerMinute;
    uint32_t windowStartMs = 0;
    uint8_t count = 0;
    bool started = false;
};
```

`src/modules/irrigation/RateLimiter.cpp`:

```cpp
#include "RateLimiter.h"

bool RateLimiter::allow(uint32_t nowMs)
{
    // Subtração unsigned: segura contra rollover de millis()
    if (!started || (nowMs - windowStartMs) >= 60000u) {
        started = true;
        windowStartMs = nowMs;
        count = 0;
    }
    if (count >= maxPerMinute)
        return false;
    count++;
    return true;
}
```

- [ ] **Step 4: Rodar e ver passar**

Run: `pio test -e native --filter test_irrigation_replay`
Expected: PASS (7 testes).

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt
git add src/modules/irrigation/SeqTable.* src/modules/irrigation/RateLimiter.* test/test_irrigation_replay/
git commit -m "feat(irrigation): add per-sender anti-replay table and command rate limiter"
```

---

### Task 3: Controlador de válvulas fail-safe (`ValveController`)

**Files:**
- Create: `src/modules/irrigation/ValveController.h`
- Create: `src/modules/irrigation/ValveController.cpp`
- Test: `test/test_irrigation_valve/test_main.cpp`

**Interfaces:**
- Consumes: nada.
- Produces: `class IValveDriver { virtual void pulse(uint8_t index, bool open) = 0; }`; `class ValveController { ValveController(IValveDriver &, uint8_t numValves); Result open(uint8_t id, uint32_t durationS, uint32_t configMaxS, uint32_t nowMs); Result close(uint8_t id); void closeAll(); void tick(uint32_t nowMs); bool isOpen(uint8_t id) const; uint8_t stateBitmap() const; void setBatteryLockout(bool); }` com `enum class Result { OK, INVALID_ID, BATTERY_LOW, ZERO_DURATION }` e `MAX_VALVES = 8`.

- [ ] **Step 1: Escrever teste que falha**

`test/test_irrigation_valve/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ValveController.h"
#include <unity.h>

class MockDriver : public IValveDriver
{
  public:
    int opens[ValveController::MAX_VALVES] = {};
    int closes[ValveController::MAX_VALVES] = {};
    void pulse(uint8_t index, bool open) override
    {
        if (open)
            opens[index]++;
        else
            closes[index]++;
    }
};

void setUp(void) {}
void tearDown(void) {}

static void test_open_pulsesOnceAndTracksState()
{
    MockDriver d;
    ValveController vc(d, 2);
    TEST_ASSERT_EQUAL(ValveController::Result::OK, vc.open(0, 60, 0, 1000));
    TEST_ASSERT_TRUE(vc.isOpen(0));
    TEST_ASSERT_EQUAL_INT(1, d.opens[0]);
    TEST_ASSERT_EQUAL_UINT8(0b01, vc.stateBitmap());
}

static void test_failsafeTimer_closesOnExpiry()
{
    MockDriver d;
    ValveController vc(d, 2);
    vc.open(0, 60, 0, 1000);
    vc.tick(1000 + 59999);
    TEST_ASSERT_TRUE(vc.isOpen(0));
    vc.tick(1000 + 60000);
    TEST_ASSERT_FALSE(vc.isOpen(0));
    TEST_ASSERT_EQUAL_INT(1, d.closes[0]);
}

static void test_renewal_restartsTimerWithoutRepulse()
{
    MockDriver d;
    ValveController vc(d, 2);
    vc.open(0, 60, 0, 1000);
    // Renovação (spec §4.2): abrir de novo estende, não repulsa
    TEST_ASSERT_EQUAL(ValveController::Result::OK, vc.open(0, 60, 0, 50000));
    TEST_ASSERT_EQUAL_INT(1, d.opens[0]);
    vc.tick(1000 + 60000); // timer antigo já teria expirado
    TEST_ASSERT_TRUE(vc.isOpen(0));
    vc.tick(50000 + 60000);
    TEST_ASSERT_FALSE(vc.isOpen(0));
}

static void test_durationClampedToCompiledCeiling()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.open(0, 999999, 0, 0); // pede ~11 dias
    vc.tick(ValveController::MAX_OPEN_SECONDS * 1000u - 1);
    TEST_ASSERT_TRUE(vc.isOpen(0));
    vc.tick(ValveController::MAX_OPEN_SECONDS * 1000u);
    TEST_ASSERT_FALSE(vc.isOpen(0)); // teto de 120 min venceu
}

static void test_configMaxTightensDuration()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.open(0, 3600, 600, 0); // config local limita a 10 min
    vc.tick(600 * 1000u);
    TEST_ASSERT_FALSE(vc.isOpen(0));
}

static void test_batteryLockout_blocksOpenAllowsClose()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.open(0, 60, 0, 0);
    vc.setBatteryLockout(true);
    TEST_ASSERT_EQUAL(ValveController::Result::BATTERY_LOW, vc.open(0, 60, 0, 1000));
    TEST_ASSERT_EQUAL(ValveController::Result::OK, vc.close(0)); // fechar sempre aceito
    TEST_ASSERT_FALSE(vc.isOpen(0));
}

static void test_invalidIdAndZeroDuration_rejected()
{
    MockDriver d;
    ValveController vc(d, 2);
    TEST_ASSERT_EQUAL(ValveController::Result::INVALID_ID, vc.open(2, 60, 0, 0));
    TEST_ASSERT_EQUAL(ValveController::Result::ZERO_DURATION, vc.open(0, 0, 0, 0));
    TEST_ASSERT_EQUAL(ValveController::Result::INVALID_ID, vc.close(5));
}

static void test_tickHandlesMillisRollover()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.open(0, 60, 0, 0xFFFFF000u); // expira depois do wrap de millis()
    vc.tick(0xFFFFFF00u);
    TEST_ASSERT_TRUE(vc.isOpen(0));
    vc.tick(0xFFFFF000u + 60000); // wrapped
    TEST_ASSERT_FALSE(vc.isOpen(0));
}

static void test_closeAll()
{
    MockDriver d;
    ValveController vc(d, 3);
    vc.open(0, 60, 0, 0);
    vc.open(2, 60, 0, 0);
    vc.closeAll();
    TEST_ASSERT_EQUAL_UINT8(0, vc.stateBitmap());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_open_pulsesOnceAndTracksState);
    RUN_TEST(test_failsafeTimer_closesOnExpiry);
    RUN_TEST(test_renewal_restartsTimerWithoutRepulse);
    RUN_TEST(test_durationClampedToCompiledCeiling);
    RUN_TEST(test_configMaxTightensDuration);
    RUN_TEST(test_batteryLockout_blocksOpenAllowsClose);
    RUN_TEST(test_invalidIdAndZeroDuration_rejected);
    RUN_TEST(test_tickHandlesMillisRollover);
    RUN_TEST(test_closeAll);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `pio test -e native --filter test_irrigation_valve`
Expected: FAIL na compilação — `ValveController.h: No such file or directory`.

- [ ] **Step 3: Implementar**

`src/modules/irrigation/ValveController.h`:

```cpp
#pragma once
#include <stdint.h>

// Abstrai a ponte H (pulso latching). Implementação GPIO fica no módulo;
// testes usam mock.
class IValveDriver
{
  public:
    virtual ~IValveDriver() = default;
    virtual void pulse(uint8_t index, bool open) = 0;
};

// Timer fail-safe local (spec §4.2): a válvula fecha sozinha ao expirar,
// independente do rádio.
class ValveController
{
  public:
    static constexpr uint8_t MAX_VALVES = 8;
    static constexpr uint32_t MAX_OPEN_SECONDS = 120 * 60; // teto compilado

    enum class Result : uint8_t { OK, INVALID_ID, BATTERY_LOW, ZERO_DURATION };

    ValveController(IValveDriver &driver, uint8_t numValves)
        : driver(driver), numValves(numValves > MAX_VALVES ? MAX_VALVES : numValves)
    {
    }

    // Efetivo = min(durationS, configMaxS quando != 0, MAX_OPEN_SECONDS).
    // Já aberta: renova o timer sem novo pulso.
    Result open(uint8_t id, uint32_t durationS, uint32_t configMaxS, uint32_t nowMs);
    Result close(uint8_t id); // sempre aceito, mesmo em lockout de bateria
    void closeAll();
    void tick(uint32_t nowMs); // fecha válvulas com timer expirado
    bool isOpen(uint8_t id) const { return id < numValves && slots[id].open; }
    uint8_t stateBitmap() const;
    void setBatteryLockout(bool locked) { batteryLockout = locked; }

  private:
    struct Slot {
        bool open = false;
        uint32_t closeAtMs = 0;
    };
    IValveDriver &driver;
    uint8_t numValves;
    bool batteryLockout = false;
    Slot slots[MAX_VALVES];
};
```

`src/modules/irrigation/ValveController.cpp`:

```cpp
#include "ValveController.h"

ValveController::Result ValveController::open(uint8_t id, uint32_t durationS, uint32_t configMaxS, uint32_t nowMs)
{
    if (id >= numValves)
        return Result::INVALID_ID;
    if (durationS == 0)
        return Result::ZERO_DURATION;
    if (batteryLockout)
        return Result::BATTERY_LOW;

    uint32_t effectiveS = durationS;
    if (configMaxS != 0 && configMaxS < effectiveS)
        effectiveS = configMaxS;
    if (effectiveS > MAX_OPEN_SECONDS)
        effectiveS = MAX_OPEN_SECONDS;

    if (!slots[id].open) {
        driver.pulse(id, true);
        slots[id].open = true;
    }
    slots[id].closeAtMs = nowMs + effectiveS * 1000u;
    return Result::OK;
}

ValveController::Result ValveController::close(uint8_t id)
{
    if (id >= numValves)
        return Result::INVALID_ID;
    if (slots[id].open) {
        driver.pulse(id, false);
        slots[id].open = false;
    }
    return Result::OK;
}

void ValveController::closeAll()
{
    for (uint8_t i = 0; i < numValves; i++)
        close(i);
}

void ValveController::tick(uint32_t nowMs)
{
    for (uint8_t i = 0; i < numValves; i++) {
        // Diferença signed: segura contra rollover de millis()
        if (slots[i].open && (int32_t)(nowMs - slots[i].closeAtMs) >= 0)
            close(i);
    }
}

uint8_t ValveController::stateBitmap() const
{
    uint8_t bm = 0;
    for (uint8_t i = 0; i < numValves; i++)
        if (slots[i].open)
            bm |= (1u << i);
    return bm;
}
```

- [ ] **Step 4: Rodar e ver passar**

Run: `pio test -e native --filter test_irrigation_valve`
Expected: PASS (9 testes).

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt
git add src/modules/irrigation/ValveController.* test/test_irrigation_valve/
git commit -m "feat(irrigation): add fail-safe valve controller with local timer"
```

---

### Task 4: Settings persistidos (`IrrigationSettings`)

**Files:**
- Create: `src/modules/irrigation/IrrigationSettings.h`
- Create: `src/modules/irrigation/IrrigationSettings.cpp`

Sem suite própria: é I/O de filesystem, coberto pelo build nativo + smoke da Task 5. Persistência atômica/NVS com epoch é a Fase 2; aqui é o mínimo para o role e o pin map sobreviverem ao reboot.

**Interfaces:**
- Consumes: `FSCommon.h` (`FSCom`, `FILE_O_READ`, `FILE_O_WRITE`) — já existe no repo.
- Produces: `enum class IrrigationRole : uint8_t { ESTACAO=0, GATEWAY=1, REPETIDOR=2, SERVICO=3 }`; `struct IrrigationSettings` (campos abaixo); `bool loadIrrigationSettings(IrrigationSettings &)`; `bool saveIrrigationSettings(const IrrigationSettings &)`.

- [ ] **Step 1: Implementar**

`src/modules/irrigation/IrrigationSettings.h`:

```cpp
#pragma once
#include <stdint.h>

enum class IrrigationRole : uint8_t { ESTACAO = 0, GATEWAY = 1, REPETIDOR = 2, SERVICO = 3 };

// Camada 1 mínima (pin map + parâmetros) da spec §5.1/§5.3. Formato completo
// (sensores, staging atômico em NVS, epoch) chega na Fase 2.
struct IrrigationSettings {
    static constexpr uint32_t MAGIC = 0x49525231; // "IRR1"
    static constexpr uint8_t MAX_VALVES = 8;

    uint32_t magic = MAGIC;
    uint16_t version = 1;
    uint8_t role = (uint8_t)IrrigationRole::ESTACAO;
    uint8_t numValves = 2;
    uint32_t boundGateway = 0; // 0 = não pareado
    uint16_t hbMinutes = 10;
    uint16_t vbatMinAbrirCentiV = 1180; // 11,8 V (spec §8.1)
    uint16_t maxOpenConfigS = 0;        // 0 = só o teto compilado limita
    uint8_t cmdRatePerMin = 10;
    uint16_t pulseMs = 60;
    int8_t pinsHbridgeA[MAX_VALVES] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int8_t pinsHbridgeB[MAX_VALVES] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// false = arquivo ausente/corrompido; `s` fica com os defaults acima.
bool loadIrrigationSettings(IrrigationSettings &s);
bool saveIrrigationSettings(const IrrigationSettings &s);
```

`src/modules/irrigation/IrrigationSettings.cpp`:

```cpp
#include "IrrigationSettings.h"
#include "DebugConfiguration.h"
#include "FSCommon.h"

static const char *SETTINGS_PATH = "/prefs/irrigation.dat";
static const char *SETTINGS_TMP = "/prefs/irrigation.tmp";

bool loadIrrigationSettings(IrrigationSettings &s)
{
#ifdef FSCom
    auto f = FSCom.open(SETTINGS_PATH, FILE_O_READ);
    if (!f)
        return false;
    IrrigationSettings tmp;
    size_t n = f.read((uint8_t *)&tmp, sizeof(tmp));
    f.close();
    if (n != sizeof(tmp) || tmp.magic != IrrigationSettings::MAGIC || tmp.version != 1) {
        LOG_WARN("Irrigation settings invalid, using defaults");
        return false;
    }
    s = tmp;
    return true;
#else
    return false;
#endif
}

bool saveIrrigationSettings(const IrrigationSettings &s)
{
#ifdef FSCom
    // Staging + rename: queda de energia no meio não corrompe o arquivo ativo
    auto f = FSCom.open(SETTINGS_TMP, FILE_O_WRITE);
    if (!f)
        return false;
    size_t n = f.write((const uint8_t *)&s, sizeof(s));
    f.close();
    if (n != sizeof(s)) {
        FSCom.remove(SETTINGS_TMP);
        return false;
    }
    FSCom.remove(SETTINGS_PATH);
    if (!renameFile(SETTINGS_TMP, SETTINGS_PATH)) {
        LOG_ERROR("Irrigation settings rename failed");
        return false;
    }
    return true;
#else
    return false;
#endif
}
```

Nota ao executor: `renameFile` está declarado em `src/FSCommon.h`. Se a assinatura divergir (`bool renameFile(const char *from, const char *to)` é a atual), ajustar a chamada — não reimplementar rename.

- [ ] **Step 2: Compilar nativo para validar**

Run: `pio run -e native` (ou `./bin/test-native-docker.sh` em host sem toolchain Linux)
Expected: build OK (arquivo ainda não referenciado por ninguém, mas compila no varredor de `src/`).

- [ ] **Step 3: Formatar e commitar**

```bash
trunk fmt
git add src/modules/irrigation/IrrigationSettings.*
git commit -m "feat(irrigation): add persisted role/pin-map settings with staged write"
```

---

### Task 5: Módulo de integração (`IrrigationModule`) + registro

**Files:**
- Create: `src/modules/irrigation/IrrigationModule.h`
- Create: `src/modules/irrigation/IrrigationModule.cpp`
- Modify: `src/modules/Modules.cpp` (include + instância em `setupModules()`)
- Modify: `test/native-suite-count` (31 → 34)

**Interfaces:**
- Consumes: tudo das Tasks 1–4; `SinglePortModule` (`allocDataPacket()`, `ourPortNum`); `service->sendToMesh(p, RX_SRC_LOCAL, false)` de `MeshService.h`; `meshtastic_PortNum_PRIVATE_APP`; `powerStatus->getBatteryVoltageMv()` (global de `PowerStatus.h`/`main.h`); `Throttle::isWithinTimespanMs` de `src/mesh/Throttle.h`.
- Produces: global `IrrigationModule *irrigationModule` (padrão dos demais módulos).

- [ ] **Step 1: Implementar o header**

`src/modules/irrigation/IrrigationModule.h`:

```cpp
#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "modules/irrigation/IrrigationSettings.h"
#include "modules/irrigation/RateLimiter.h"
#include "modules/irrigation/SeqTable.h"
#include "modules/irrigation/ValveController.h"

// Ponte H latching: pulso em A abre, pulso em B fecha.
class GpioValveDriver : public IValveDriver
{
  public:
    void configure(const IrrigationSettings &s);
    void pulse(uint8_t index, bool open) override;

  private:
    int8_t pinsA[IrrigationSettings::MAX_VALVES];
    int8_t pinsB[IrrigationSettings::MAX_VALVES];
    uint16_t pulseMs = 60;
};

class IrrigationModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    IrrigationModule();

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    void handleCmdValvula(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void sendAck(uint32_t to, uint32_t ackedSeq, uint8_t status, uint8_t reason);
    void sendHeartbeat();
    bool senderAuthorized(uint32_t from) const;
    uint16_t batteryCentiV() const;

    IrrigationSettings settings;
    GpioValveDriver driver;
    ValveController valves;
    SeqTable seqTable;
    RateLimiter rateLimiter;
    uint32_t txSeq = 0;
    uint32_t lastHeartbeatMs = 0;
};

extern IrrigationModule *irrigationModule;
```

- [ ] **Step 2: Implementar o cpp**

`src/modules/irrigation/IrrigationModule.cpp`:

```cpp
#include "modules/irrigation/IrrigationModule.h"
#include "MeshService.h"
#include "PowerStatus.h"
#include "Throttle.h"
#include "configuration.h"
#include "main.h"

IrrigationModule *irrigationModule;

using namespace IrrigationProto;

void GpioValveDriver::configure(const IrrigationSettings &s)
{
    memcpy(pinsA, s.pinsHbridgeA, sizeof(pinsA));
    memcpy(pinsB, s.pinsHbridgeB, sizeof(pinsB));
    pulseMs = s.pulseMs;
#ifndef ARCH_PORTDUINO
    for (uint8_t i = 0; i < IrrigationSettings::MAX_VALVES; i++) {
        if (pinsA[i] >= 0) {
            pinMode(pinsA[i], OUTPUT);
            digitalWrite(pinsA[i], LOW);
        }
        if (pinsB[i] >= 0) {
            pinMode(pinsB[i], OUTPUT);
            digitalWrite(pinsB[i], LOW);
        }
    }
#endif
}

void GpioValveDriver::pulse(uint8_t index, bool open)
{
    int8_t pin = open ? pinsA[index] : pinsB[index];
    if (pin < 0) {
        LOG_WARN("Valve %d has no %s pin mapped", index, open ? "open" : "close");
        return;
    }
#ifndef ARCH_PORTDUINO
    digitalWrite(pin, HIGH);
    delay(pulseMs);
    digitalWrite(pin, LOW);
#endif
    LOG_INFO("Valve %d pulsed %s", index, open ? "OPEN" : "CLOSE");
}

IrrigationModule::IrrigationModule()
    : SinglePortModule("irrigation", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Irrigation"),
      valves(driver, 0), rateLimiter(10)
{
    loadIrrigationSettings(settings);
    driver.configure(settings);
    valves = ValveController(driver, settings.numValves);
    rateLimiter = RateLimiter(settings.cmdRatePerMin);
    // Boot sempre em estado fechado (spec §5.5): solenoides latching podem ter
    // ficado abertos num reset com válvula acionada.
    valves.closeAll();
    LOG_INFO("IrrigationModule role=%d valves=%d gateway=0x%08x", settings.role, settings.numValves, settings.boundGateway);
}

bool IrrigationModule::wantPacket(const meshtastic_MeshPacket *p)
{
    if ((IrrigationRole)settings.role == IrrigationRole::REPETIDOR)
        return false; // repetidor só retransmite (spec §3.1)
    return p->decoded.portnum == ourPortNum;
}

bool IrrigationModule::senderAuthorized(uint32_t from) const
{
    // Fase 3 (pareamento) elimina o modo aberto: hoje, sem vínculo gravado,
    // qualquer nó do canal comanda (posse da PSK = autoridade, spec §4.2).
    return settings.boundGateway == 0 || from == settings.boundGateway;
}

ProcessMessage IrrigationModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    const auto &d = mp.decoded;
    Header h;
    if (!decodeHeader(d.payload.bytes, d.payload.size, h)) {
        LOG_WARN("Irrigation: runt packet from 0x%08x", mp.from);
        return ProcessMessage::STOP;
    }
    if (h.version != VERSION) {
        LOG_WARN("Irrigation: protocol version %d != %d from 0x%08x", h.version, VERSION, mp.from);
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_VERSION);
        return ProcessMessage::STOP;
    }

    switch (h.type) {
    case MSG_CMD_VALVULA:
        handleCmdValvula(mp, h);
        break;
    case MSG_ACK:
    case MSG_HEARTBEAT:
        // Lado gateway chega na Fase 4; por ora só loga
        LOG_DEBUG("Irrigation: type %d from 0x%08x (ignored, role=%d)", h.type, mp.from, settings.role);
        break;
    default:
        LOG_DEBUG("Irrigation: unhandled type %d from 0x%08x", h.type, mp.from);
        break;
    }
    return ProcessMessage::STOP;
}

void IrrigationModule::handleCmdValvula(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from)) {
        LOG_WARN("Irrigation: unauthorized cmd from 0x%08x", mp.from);
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        return;
    }
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: replayed seq %u from 0x%08x", h.seq, mp.from);
        return; // replay: descarta em silêncio, não ACKa
    }
    if (!rateLimiter.allow(millis())) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_RATE_LIMIT);
        return;
    }

    CmdValvula cmd;
    if (!decodeCmdValvula(mp.decoded.payload.bytes, mp.decoded.payload.size, cmd)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        return;
    }

    ValveController::Result r;
    if (cmd.action == 1)
        r = valves.open(cmd.valveId, cmd.durationS, settings.maxOpenConfigS, millis());
    else
        r = valves.close(cmd.valveId);

    uint8_t reason = REASON_NONE;
    switch (r) {
    case ValveController::Result::OK:
        break;
    case ValveController::Result::INVALID_ID:
    case ValveController::Result::ZERO_DURATION:
        reason = REASON_INVALID_ID;
        break;
    case ValveController::Result::BATTERY_LOW:
        reason = REASON_BATTERY_LOW;
        break;
    }
    sendAck(mp.from, h.seq, reason == REASON_NONE ? ACK_OK : ACK_NACK, reason);
}

void IrrigationModule::sendAck(uint32_t to, uint32_t ackedSeq, uint8_t status, uint8_t reason)
{
    Ack ack = {};
    ack.ackedSeq = ackedSeq;
    ack.status = status;
    ack.reason = reason;
    ack.valveStates = valves.stateBitmap();
    ack.vbatCentiV = batteryCentiV();
    ack.configEpoch = 0; // epoch chega na Fase 2

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = to;
    p->decoded.payload.size = encodeAck(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, ack);
    service->sendToMesh(p, RX_SRC_LOCAL, false);
}

void IrrigationModule::sendHeartbeat()
{
    Heartbeat hb = {};
    hb.valveStates = valves.stateBitmap();
    hb.vbatCentiV = batteryCentiV();
    hb.configEpoch = 0;

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = settings.boundGateway ? settings.boundGateway : NODENUM_BROADCAST;
    p->decoded.payload.size = encodeHeartbeat(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, hb);
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    LOG_DEBUG("Irrigation heartbeat sent, valves=0x%x vbat=%u cV", hb.valveStates, hb.vbatCentiV);
}

uint16_t IrrigationModule::batteryCentiV() const
{
    if (powerStatus && powerStatus->getHasBattery())
        return powerStatus->getBatteryVoltageMv() / 10;
    return 0;
}

int32_t IrrigationModule::runOnce()
{
    if ((IrrigationRole)settings.role != IrrigationRole::ESTACAO)
        return 60 * 1000; // gateway/repetidor/serviço: nada periódico nesta fase

    valves.tick(millis());
    valves.setBatteryLockout(batteryCentiV() != 0 && batteryCentiV() < settings.vbatMinAbrirCentiV);

    if (!Throttle::isWithinTimespanMs(lastHeartbeatMs, (uint32_t)settings.hbMinutes * 60 * 1000)) {
        lastHeartbeatMs = millis();
        sendHeartbeat();
    }
    return 1000; // tick de 1 s mantém o fail-safe responsivo
}
```

Nota ao executor: confirmar assinatura de `Throttle::isWithinTimespanMs` em `src/mesh/Throttle.h` e o global `powerStatus` (declarado em `src/main.h` como `extern meshtastic::PowerStatus *powerStatus;` — se o nome/namespace divergir, seguir o uso em `src/modules/Telemetry/DeviceTelemetry.cpp`). `NODENUM_BROADCAST` vem de `MeshTypes.h` via `SinglePortModule.h`.

- [ ] **Step 3: Registrar em `Modules.cpp`**

Em `src/modules/Modules.cpp`, junto aos outros includes no topo:

```cpp
#if !MESHTASTIC_EXCLUDE_IRRIGATION
#include "modules/irrigation/IrrigationModule.h"
#endif
```

Dentro de `setupModules()`, junto às demais instanciações incondicionais (perto de `new RoutingModule()` ou similar):

```cpp
#if !MESHTASTIC_EXCLUDE_IRRIGATION
    irrigationModule = new IrrigationModule();
#endif
```

- [ ] **Step 4: Atualizar contagem de suites**

`test/native-suite-count`: trocar `31` por `34` (3 suites novas: protocol, replay, valve).

- [ ] **Step 5: Build + suíte completa**

Run: `pio run -e native` — Expected: build OK.
Run: `./bin/run-tests.sh` — Expected: exit 0 (GREEN), 34 suites.

- [ ] **Step 6: Build de hardware (smoke de compilação ESP32)**

Run: `pio run -e tbeam`
Expected: build OK (valida pinMode/digitalWrite/delay no caminho ARCH ESP32).

- [ ] **Step 7: Formatar e commitar**

```bash
trunk fmt
git add src/modules/irrigation/IrrigationModule.* src/modules/Modules.cpp test/native-suite-count
git commit -m "feat(irrigation): add station module with fail-safe valve command handling"
```

---

## Self-Review (executado na escrita do plano)

- **Cobertura da spec (escopo Fase 1)**: §4.1 CMD_VALVULA/ACK/HEARTBEAT/CMD_GPO codec ✔ (GPO handling no módulo fica p/ Fase 6 — só o codec entra agora); §4.2 fail-safe/renovação/anti-replay/rate-limit/bateria ✔; §3.1 role + versão de protocolo ✔; §3.2 portnum privado + payload compacto ✔. Restante da spec: fases 2–9 (ver roadmap).
- **Placeholders**: nenhum "TBD"; os dois avisos ao executor apontam verificação de assinatura existente, não código faltante.
- **Consistência de tipos**: `Result::ZERO_DURATION` mapeada para `REASON_INVALID_ID` no ACK (decisão: receptor não distingue p/ o remetente); `close(id)` sem `nowMs` — assinatura igual nas Tasks 3 e 5; `stateBitmap()`/`checkAndUpdate()`/`allow()` idênticos entre definição e uso.
