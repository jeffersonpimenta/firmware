# Irrigação Mesh — Fase 2: Config remota com epoch, fragmentação e modo seguro

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Estação totalmente configurável pelo rádio: `SET_CONFIG` fragmentado com CRC32 e commit atômico (ACK só depois de gravado), `GET_CONFIG`, epoch reportado em todo ACK/heartbeat, modo seguro quando a config local está ausente/corrompida, e formato de settings versionado (v1→v2) com polaridade configurável de entradas digitais.

**Architecture:** O blob de config no rádio é o MESMO formato do disco: a struct `IrrigationSettings` v2 (LE, ABI travada por `static_assert`). `SET_CONFIG` transporta o blob em fragmentos ≤160 B; um `FragmentReassembler` (lógica pura) remonta e valida CRC32; o módulo aplica preservando `role`/`boundGateway` (credenciais são do pareamento, Fase 3), grava com staging+rename e só então ACKa com o novo epoch. `GET_CONFIG` responde com o blob vigente no mesmo formato de fragmento. Load que falha ⇒ modo seguro (abrir bloqueado, fechar e `SET_CONFIG` aceitos — o push de config é a rota de recuperação).

**Tech Stack:** C++17, Unity nativo via Docker (`MSYS_NO_PATHCONV=1 ./bin/test-native-docker.sh` — neste host usar o comando docker com volumes `pio-cache`/`fwtest-cache` já estabelecido), módulos da Fase 1 em `src/modules/irrigation/`.

## Global Constraints

- Payload ≤ **200 bytes** (`IrrigationProto::MAX_PAYLOAD`); header de 8 B; fragmento de dados ≤ **160 B** (`FRAG_DATA_MAX`).
- **ACK de config só após commit em NVS/arquivo** (spec §5.5: ACK = gravado, não recebido).
- Config remota **nunca altera `role` nem `boundGateway`** (credenciais/identidade são do provisionamento e do pareamento da Fase 3).
- NVS vazia/corrompida ⇒ **modo seguro**: recusa abrir (NACK `REASON_SAFE_MODE`), aceita fechar e `SET_CONFIG`; flag `HB_FLAG_SAFE_MODE` em todo heartbeat (spec §5.5).
- Config máxima: **512 B** (`FragmentReassembler::MAX_CONFIG_LEN`); até 16 fragmentos.
- CRC32 padrão refletido (poly `0xEDB88320`, init `0xFFFFFFFF`, xorout `0xFFFFFFFF`); vetor de verificação: `crc32("123456789") == 0xCBF43926`.
- Settings v2 = v1 + `configEpoch` (u32) + `pinsDigitalIn[4]` (i8, -1 = ausente) + `digitalInActiveLow` (bitmask u8, bit i = entrada i ativo-baixo — requisito do usuário: polaridade configurável). Migração v1→v2 automática no load; **nunca** perder campos v1.
- Anti-replay/rate-limit continuam valendo para mensagens de config (cada fragmento tem seq próprio e consome rate budget).
- Estilo: LOG_* com node IDs `0x%08x`; comentários mínimos; C++17; nada em `src/mesh/generated/`.
- Testes: padrão da Fase 1 — `#include "Arduino.h"` PRIMEIRO include de todo test_main.cpp (linkage extern-C de setup/loop no Portduino); `test/native-suite-count` vai de **34 → 36** (Task 5, nunca antes).
- Rodar teste no host Windows (Docker, ~3 min por suite):

```
MSYS_NO_PATHCONV=1 docker run --rm --name irrig-p2 -u 0 -e HOME=/root -v "$(pwd -W 2>/dev/null || pwd):/src:ro" -v pio-cache:/root -v fwtest-cache:/tmp/fw-test meshtastic-native-test timeout -k 30 1200 bash -c "cp -a /src/src/. /tmp/fw-test/src/ && cp -a /src/test/. /tmp/fw-test/test/ && cd /tmp/fw-test && platformio test -e coverage -f <SUITE> -vv 2>&1 | grep -E ':PASS|:FAIL|PASSED|FAILED|ERRORED|undefined|error' | head -40"
```

## File Structure

```
src/modules/irrigation/
  IrrigationProtocol.h/.cpp     # MODIFY: crc32, SetConfig codec, novos NackReasons (Task 1)
  IrrigationSettings.h/.cpp     # MODIFY: v2 + migrateIrrigationSettings pura (Task 2)
  FragmentReassembler.h/.cpp    # CREATE: remontagem com CRC e timeout (Task 3)
  ValveController.h/.cpp        # MODIFY: setNumValves com force-close (Task 4)
  IrrigationModule.h/.cpp       # MODIFY: handlers SET/GET_CONFIG, safe mode, epoch, boot HB (Task 4)
test/test_irrigation_protocol/test_main.cpp  # MODIFY (Task 1)
test/test_irrigation_config/test_main.cpp    # CREATE (Task 2)
test/test_irrigation_fragment/test_main.cpp  # CREATE (Task 3)
test/test_irrigation_valve/test_main.cpp     # MODIFY (Task 4)
test/native-suite-count                      # 34 → 36 (Task 5)
```

---

### Task 1: CRC32 + codec de `SET_CONFIG` + reasons novos + asserts pendentes

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h`
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp`
- Test: `test/test_irrigation_protocol/test_main.cpp` (estender)

**Interfaces:**
- Consumes: codec da Fase 1 (Writer/Reader internos, `HEADER_LEN`, `writeHeader`).
- Produces: `uint32_t IrrigationProto::crc32(const uint8_t *data, size_t len)`; `constexpr uint8_t FRAG_DATA_MAX = 160`; `struct SetConfig { uint32_t epoch; uint32_t crc; uint16_t totalLen; uint8_t fragIndex; uint8_t fragCount; uint8_t fragLen; const uint8_t *frag; }`; `size_t encodeSetConfig(uint8_t *buf, size_t len, uint32_t seq, const SetConfig &m)` (0 = erro/fragLen>FRAG_DATA_MAX); `bool decodeSetConfig(const uint8_t *buf, size_t len, SetConfig &out)` (out.frag aponta DENTRO de buf); `size_t encodeGetConfig(uint8_t *buf, size_t len, uint32_t seq)` (só header, tipo MSG_GET_CONFIG); novos reasons `REASON_SAFE_MODE = 7`, `REASON_BAD_CRC = 8`, `REASON_CONFIG_TOO_BIG = 9`, `REASON_FRAG_INVALID = 10`, `REASON_COMMIT_FAIL = 11`.

- [ ] **Step 1: Estender a suite com testes que falham**

Acrescentar a `test/test_irrigation_protocol/test_main.cpp` (antes de `setup()`), e registrar os `RUN_TEST` novos em `setup()`:

```cpp
static void test_crc32_referenceVector()
{
    const uint8_t v[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(v, 9));
    TEST_ASSERT_EQUAL_HEX32(0x00000000 ^ 0xFFFFFFFF ^ 0xFFFFFFFF, crc32(v, 0) ^ 0); // len 0 = 0x00000000
}

static void test_setConfig_roundTrip()
{
    uint8_t blob[100];
    for (int i = 0; i < 100; i++)
        blob[i] = (uint8_t)i;

    uint8_t buf[MAX_PAYLOAD];
    SetConfig in = {};
    in.epoch = 18;
    in.crc = crc32(blob, 100);
    in.totalLen = 100;
    in.fragIndex = 1;
    in.fragCount = 2;
    in.fragLen = 60;
    in.frag = blob + 40;
    size_t n = encodeSetConfig(buf, sizeof(buf), 9, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_SET_CONFIG, h.type);

    SetConfig out;
    TEST_ASSERT_TRUE(decodeSetConfig(buf, n, out));
    TEST_ASSERT_EQUAL_UINT32(18, out.epoch);
    TEST_ASSERT_EQUAL_HEX32(in.crc, out.crc);
    TEST_ASSERT_EQUAL_UINT16(100, out.totalLen);
    TEST_ASSERT_EQUAL_UINT8(1, out.fragIndex);
    TEST_ASSERT_EQUAL_UINT8(2, out.fragCount);
    TEST_ASSERT_EQUAL_UINT8(60, out.fragLen);
    TEST_ASSERT_EQUAL_MEMORY(blob + 40, out.frag, 60);
}

static void test_setConfig_fragTooBig_rejected()
{
    uint8_t blob[FRAG_DATA_MAX + 1] = {0};
    uint8_t buf[MAX_PAYLOAD];
    SetConfig in = {};
    in.totalLen = sizeof(blob);
    in.fragCount = 1;
    in.fragLen = FRAG_DATA_MAX + 1;
    in.frag = blob;
    TEST_ASSERT_EQUAL_UINT(0, encodeSetConfig(buf, sizeof(buf), 1, in));
}

static void test_setConfig_truncated_rejected()
{
    uint8_t blob[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint8_t buf[MAX_PAYLOAD];
    SetConfig in = {};
    in.totalLen = 10;
    in.fragCount = 1;
    in.fragLen = 10;
    in.frag = blob;
    size_t n = encodeSetConfig(buf, sizeof(buf), 1, in);
    SetConfig out;
    TEST_ASSERT_FALSE(decodeSetConfig(buf, n - 1, out)); // corta 1 byte do frag
}

static void test_getConfig_headerOnly()
{
    uint8_t buf[MAX_PAYLOAD];
    size_t n = encodeGetConfig(buf, sizeof(buf), 3);
    TEST_ASSERT_EQUAL_UINT(HEADER_LEN, n);
    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_GET_CONFIG, h.type);
    TEST_ASSERT_EQUAL_UINT32(3, h.seq);
}
```

E completar os asserts pendentes da revisão da Fase 1 (dentro dos testes existentes):

```cpp
// em test_ack_roundTrip, junto aos demais asserts:
    TEST_ASSERT_EQUAL_UINT8(0, out.gpoStates);
// em test_heartbeat_roundTrip, junto aos demais asserts:
    TEST_ASSERT_EQUAL_UINT16(1810, out.vpanelCentiV);
    TEST_ASSERT_EQUAL_UINT8(2, out.rebootCause);
```

- [ ] **Step 2: Rodar e ver falhar**

Run (comando global, `<SUITE>` = `test_irrigation_protocol`)
Expected: FAIL de compilação — `crc32`/`SetConfig` não declarados.

- [ ] **Step 3: Implementar**

Em `IrrigationProtocol.h`, dentro do namespace, acrescentar (após os enums existentes — NÃO renumerar nada existente):

```cpp
constexpr uint8_t FRAG_DATA_MAX = 160;

// acrescentar ao enum NackReason (mantendo os valores existentes):
    REASON_SAFE_MODE = 7,
    REASON_BAD_CRC = 8,
    REASON_CONFIG_TOO_BIG = 9,
    REASON_FRAG_INVALID = 10,
    REASON_COMMIT_FAIL = 11,

struct SetConfig {
    uint32_t epoch;
    uint32_t crc;      // crc32 do blob completo
    uint16_t totalLen; // bytes do blob completo
    uint8_t fragIndex;
    uint8_t fragCount;
    uint8_t fragLen;
    const uint8_t *frag; // decode: aponta dentro do buffer de entrada
};

uint32_t crc32(const uint8_t *data, size_t len);
size_t encodeSetConfig(uint8_t *buf, size_t len, uint32_t seq, const SetConfig &m);
bool decodeSetConfig(const uint8_t *buf, size_t len, SetConfig &out);
size_t encodeGetConfig(uint8_t *buf, size_t len, uint32_t seq);
```

Em `IrrigationProtocol.cpp`, acrescentar:

```cpp
uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

size_t encodeSetConfig(uint8_t *buf, size_t len, uint32_t seq, const SetConfig &m)
{
    if (m.fragLen > FRAG_DATA_MAX)
        return 0;
    Writer w{buf, len};
    writeHeader(w, MSG_SET_CONFIG, seq);
    w.u32(m.epoch);
    w.u32(m.crc);
    w.u16(m.totalLen);
    w.u8(m.fragIndex);
    w.u8(m.fragCount);
    w.u8(m.fragLen);
    for (uint8_t i = 0; w.ok && i < m.fragLen; i++)
        w.u8(m.frag[i]);
    return w.ok ? w.pos : 0;
}

bool decodeSetConfig(const uint8_t *buf, size_t len, SetConfig &out)
{
    Reader r = bodyReader(buf, len);
    out.epoch = r.u32();
    out.crc = r.u32();
    out.totalLen = r.u16();
    out.fragIndex = r.u8();
    out.fragCount = r.u8();
    out.fragLen = r.u8();
    if (!r.ok || out.fragLen > FRAG_DATA_MAX || r.pos + out.fragLen > len)
        return false;
    out.frag = buf + r.pos;
    return true;
}

size_t encodeGetConfig(uint8_t *buf, size_t len, uint32_t seq)
{
    Writer w{buf, len};
    writeHeader(w, MSG_GET_CONFIG, seq);
    return w.ok ? w.pos : 0;
}
```

- [ ] **Step 4: Rodar e ver passar**

Run: suite `test_irrigation_protocol`
Expected: PASS (11 testes: 6 antigos com asserts extras + 5 novos).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.* test/test_irrigation_protocol/
git commit -m "feat(irrigation): add config transfer codec with crc32 and new nack reasons"
```

---

### Task 2: Settings v2 + migração v1→v2 (`migrateIrrigationSettings`)

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h`
- Modify: `src/modules/irrigation/IrrigationSettings.cpp`
- Test: `test/test_irrigation_config/test_main.cpp` (nova suite)

**Interfaces:**
- Consumes: struct v1 existente (40 B, layout no comentário do header).
- Produces: struct v2 (52 B) com `uint32_t configEpoch`, `int8_t pinsDigitalIn[4]`, `uint8_t digitalInActiveLow` (bitmask); `constexpr size_t IRRIGATION_SETTINGS_V1_SIZE = 40;` `bool migrateIrrigationSettings(const uint8_t *raw, size_t n, IrrigationSettings &out)` — pura, aceita blob v1 ou v2, false para magic/versão/tamanho inválido; `loadIrrigationSettings` passa a ler bytes do arquivo e delegar à migração (persistindo v2 de volta quando migrou de v1).

- [ ] **Step 1: Escrever a suite que falha**

`test/test_irrigation_config/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationSettings.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// Monta uma imagem v1 (40 B) byte a byte, no layout documentado no header da v1.
static void buildV1Image(uint8_t *img)
{
    memset(img, 0, IRRIGATION_SETTINGS_V1_SIZE);
    uint32_t magic = IrrigationSettings::MAGIC;
    uint16_t version = 1;
    memcpy(img + 0, &magic, 4);
    memcpy(img + 4, &version, 2);
    img[6] = 1;                       // role = GATEWAY
    img[7] = 4;                       // numValves
    uint32_t gw = 0xa1b2c3d4;
    memcpy(img + 8, &gw, 4);          // boundGateway
    uint16_t hb = 15, vbat = 1200, maxOpen = 600;
    memcpy(img + 12, &hb, 2);
    memcpy(img + 14, &vbat, 2);
    memcpy(img + 16, &maxOpen, 2);
    img[18] = 6;                      // cmdRatePerMin  (offset 19 = padding)
    uint16_t pulse = 80;
    memcpy(img + 20, &pulse, 2);
    for (int i = 0; i < 8; i++) {
        img[22 + i] = (uint8_t)(int8_t)(i < 4 ? 10 + i : -1); // pinsHbridgeA
        img[30 + i] = (uint8_t)(int8_t)(i < 4 ? 20 + i : -1); // pinsHbridgeB
    }
}

static void test_migrate_v1_preservesFieldsAndDefaultsNew()
{
    uint8_t img[IRRIGATION_SETTINGS_V1_SIZE];
    buildV1Image(img);
    IrrigationSettings s;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(img, sizeof(img), s));
    TEST_ASSERT_EQUAL_UINT16(2, s.version);
    TEST_ASSERT_EQUAL_UINT8(1, s.role);
    TEST_ASSERT_EQUAL_UINT8(4, s.numValves);
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, s.boundGateway);
    TEST_ASSERT_EQUAL_UINT16(15, s.hbMinutes);
    TEST_ASSERT_EQUAL_UINT16(1200, s.vbatMinAbrirCentiV);
    TEST_ASSERT_EQUAL_UINT16(600, s.maxOpenConfigS);
    TEST_ASSERT_EQUAL_UINT8(6, s.cmdRatePerMin);
    TEST_ASSERT_EQUAL_UINT16(80, s.pulseMs);
    TEST_ASSERT_EQUAL_INT8(12, s.pinsHbridgeA[2]);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsHbridgeA[7]);
    TEST_ASSERT_EQUAL_INT8(22, s.pinsHbridgeB[2]);
    // Campos novos: defaults
    TEST_ASSERT_EQUAL_UINT32(0, s.configEpoch);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsDigitalIn[0]);
    TEST_ASSERT_EQUAL_UINT8(0, s.digitalInActiveLow);
}

static void test_migrate_v2_passthrough()
{
    IrrigationSettings in;
    in.configEpoch = 17;
    in.pinsDigitalIn[1] = 36;
    in.digitalInActiveLow = 0b0010;
    in.numValves = 3;
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings((const uint8_t *)&in, sizeof(in), out));
    TEST_ASSERT_EQUAL_UINT32(17, out.configEpoch);
    TEST_ASSERT_EQUAL_INT8(36, out.pinsDigitalIn[1]);
    TEST_ASSERT_EQUAL_UINT8(0b0010, out.digitalInActiveLow);
    TEST_ASSERT_EQUAL_UINT8(3, out.numValves);
}

static void test_migrate_rejectsBadInput()
{
    IrrigationSettings s;
    uint8_t img[IRRIGATION_SETTINGS_V1_SIZE];
    buildV1Image(img);

    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, 10, s)); // curto

    img[0] ^= 0xFF; // magic errado
    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, sizeof(img), s));
    img[0] ^= 0xFF;

    img[4] = 3; // versão desconhecida
    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, sizeof(img), s));
}

static void test_migrate_v2_wrongSize_rejected()
{
    IrrigationSettings in;
    TEST_ASSERT_FALSE(migrateIrrigationSettings((const uint8_t *)&in, sizeof(in) - 1, IrrigationSettings{} = in));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_migrate_v1_preservesFieldsAndDefaultsNew);
    RUN_TEST(test_migrate_v2_passthrough);
    RUN_TEST(test_migrate_rejectsBadInput);
    RUN_TEST(test_migrate_v2_wrongSize_rejected);
    exit(UNITY_END());
}

void loop() {}
```

Nota ao executor: `test_migrate_v2_wrongSize_rejected` como escrito acima tem uma expressão inválida no 3º argumento — corrigir para uma variável local `IrrigationSettings out;` e passar `out`. (Deixado explícito para não ser transcrito às cegas.)

- [ ] **Step 2: Rodar e ver falhar**

Run: suite `test_irrigation_config`
Expected: FAIL de compilação — `migrateIrrigationSettings`/`IRRIGATION_SETTINGS_V1_SIZE`/campos v2 inexistentes.

- [ ] **Step 3: Implementar**

`IrrigationSettings.h` — alterar a struct para v2 (campos novos NO FIM, antes do fechamento) e atualizar o assert:

```cpp
struct IrrigationSettings {
    static constexpr uint32_t MAGIC = 0x49525231; // "IRR1"
    static constexpr uint8_t MAX_VALVES = 8;
    static constexpr uint8_t MAX_DIGITAL_IN = 4;

    uint32_t magic = MAGIC;
    uint16_t version = 2;
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
    // v2:
    uint32_t configEpoch = 0;                        // spec §5.4
    int8_t pinsDigitalIn[MAX_DIGITAL_IN] = {-1, -1, -1, -1};
    uint8_t digitalInActiveLow = 0; // bitmask: bit i = entrada i ativo-baixo (polaridade configurável)
};

constexpr size_t IRRIGATION_SETTINGS_V1_SIZE = 40;

// ABI lock v2: v1(40) + configEpoch(4) + pinsDigitalIn(4) + digitalInActiveLow(1) + pad(3) = 52.
// Bump version E este assert em qualquer mudança de layout.
static_assert(sizeof(IrrigationSettings) == 52, "on-disk settings format is ABI-dependent; bump version on layout change");

// Blob v1 ou v2 → struct v2. false = magic/versão/tamanho inválido (out fica intacto).
bool migrateIrrigationSettings(const uint8_t *raw, size_t n, IrrigationSettings &out);
```

`IrrigationSettings.cpp` — implementar a migração e reusar no load:

```cpp
bool migrateIrrigationSettings(const uint8_t *raw, size_t n, IrrigationSettings &out)
{
    uint32_t magic;
    uint16_t version;
    if (n < 6)
        return false;
    memcpy(&magic, raw, 4);
    memcpy(&version, raw + 4, 2);
    if (magic != IrrigationSettings::MAGIC)
        return false;

    if (version == 2) {
        if (n != sizeof(IrrigationSettings))
            return false;
        memcpy(&out, raw, sizeof(out));
        return true;
    }
    if (version == 1) {
        if (n != IRRIGATION_SETTINGS_V1_SIZE)
            return false;
        IrrigationSettings s; // defaults v2 para os campos novos
        memcpy(&s, raw, IRRIGATION_SETTINGS_V1_SIZE); // layout v1 é prefixo do v2
        s.version = 2;
        out = s;
        return true;
    }
    return false;
}
```

E trocar o corpo de `loadIrrigationSettings` para ler os bytes e delegar (mantendo `#ifdef FSCom`):

```cpp
bool loadIrrigationSettings(IrrigationSettings &s)
{
#ifdef FSCom
    auto f = FSCom.open(SETTINGS_PATH, FILE_O_READ);
    if (!f)
        return false;
    uint8_t raw[sizeof(IrrigationSettings)];
    size_t n = f.read(raw, sizeof(raw));
    f.close();
    IrrigationSettings tmp;
    if (!migrateIrrigationSettings(raw, n, tmp)) {
        LOG_WARN("Irrigation settings invalid (len=%u), using defaults", (unsigned)n);
        return false;
    }
    bool migrated = (n == IRRIGATION_SETTINGS_V1_SIZE);
    s = tmp;
    if (migrated)
        saveIrrigationSettings(s); // regrava já em v2
    return true;
#else
    return false;
#endif
}
```

Atenção: a leitura `f.read(raw, sizeof(raw))` lê no máximo 52 B; um arquivo v1 de 40 B retorna `n == 40` e migra. Um arquivo v2 íntegro retorna 52.

- [ ] **Step 4: Rodar e ver passar**

Run: suite `test_irrigation_config`
Expected: PASS (4 testes). Rodar também `test_irrigation_valve` (compila todo o src — garante que o resto não quebrou com a struct nova).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationSettings.* test/test_irrigation_config/
git commit -m "feat(irrigation): version settings format to v2 with epoch and digital-input polarity"
```

---

### Task 3: `FragmentReassembler`

**Files:**
- Create: `src/modules/irrigation/FragmentReassembler.h`
- Create: `src/modules/irrigation/FragmentReassembler.cpp`
- Test: `test/test_irrigation_fragment/test_main.cpp` (nova suite)

**Interfaces:**
- Consumes: `IrrigationProto::crc32` (Task 1).
- Produces:

```cpp
class FragmentReassembler
{
  public:
    static constexpr size_t MAX_CONFIG_LEN = 512;
    static constexpr uint8_t MAX_FRAGS = 16;
    static constexpr uint32_t TRANSFER_TIMEOUT_MS = 30000;

    enum class Add : uint8_t { ACCEPTED, COMPLETE, TOO_BIG, BAD_CRC, INVALID };

    Add add(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen, uint8_t fragIndex, uint8_t fragCount,
            const uint8_t *data, uint8_t fragLen, uint32_t nowMs);
    const uint8_t *blob() const { return buf; }
    uint16_t blobLen() const { return curTotalLen; }
    uint32_t epoch() const { return curEpoch; }
    void reset();
};
```

Semântica de `add`: transferência única em curso, chaveada por (sender, epoch, crc, totalLen, fragCount). Novo conjunto de chaves — ou timeout de 30 s desde o último fragmento — descarta a transferência anterior e começa outra. Duplicata de fragmento = ACCEPTED (idempotente). `TOO_BIG` para `totalLen > MAX_CONFIG_LEN` ou `fragCount > MAX_FRAGS`; `INVALID` para índice ≥ fragCount, fragLen 0, ou soma de fragmentos incompatível com totalLen; quando o último fragmento chega, valida `crc32(blob, totalLen)`: bate → COMPLETE (blob fica disponível até `reset()`/próxima transferência), não bate → BAD_CRC + reset interno.

- [ ] **Step 1: Escrever a suite que falha**

`test/test_irrigation_fragment/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/FragmentReassembler.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include <string.h>
#include <unity.h>

using Add = FragmentReassembler::Add;

void setUp(void) {}
void tearDown(void) {}

static uint8_t blob[300];
static uint32_t blobCrc;

static void makeBlob()
{
    for (int i = 0; i < 300; i++)
        blob[i] = (uint8_t)(i * 7);
    blobCrc = IrrigationProto::crc32(blob, 300);
}

// 300 B em 2 fragmentos de 160 + 140.
static Add feed(FragmentReassembler &r, uint8_t idx, uint32_t nowMs, uint32_t sender = 1, uint32_t epoch = 5)
{
    uint16_t off = idx * 160;
    uint8_t len = (idx == 0) ? 160 : 140;
    return r.add(sender, epoch, blobCrc, 300, idx, 2, blob + off, len, nowMs);
}

static void test_inOrder_completesWithMatchingBlob()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1000));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 1, 2000));
    TEST_ASSERT_EQUAL_UINT16(300, r.blobLen());
    TEST_ASSERT_EQUAL_UINT32(5, r.epoch());
    TEST_ASSERT_EQUAL_MEMORY(blob, r.blob(), 300);
}

static void test_outOfOrder_completes()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 1, 1000));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 0, 2000));
    TEST_ASSERT_EQUAL_MEMORY(blob, r.blob(), 300);
}

static void test_duplicateFragment_idempotent()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1000));
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1500));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 1, 2000));
}

static void test_badCrc_rejectedAndReset()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, r.add(1, 5, blobCrc ^ 1, 300, 0, 2, blob, 160, 1000));
    TEST_ASSERT_EQUAL(Add::BAD_CRC, r.add(1, 5, blobCrc ^ 1, 300, 1, 2, blob + 160, 140, 2000));
    // após BAD_CRC a transferência morre; recomeço do zero funciona
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 3000));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 1, 4000));
}

static void test_newKeyDropsOldTransfer()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1000, /*sender=*/1));
    // outro remetente começa: transferência antiga descartada
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1500, /*sender=*/2));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 1, 2000, /*sender=*/2));
}

static void test_timeoutDropsStaleTransfer()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1000));
    // 31 s depois só chegou o frag 1: é tratado como transferência nova (incompleta)
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 1, 1000 + 31000));
    // e o frag 0 completa
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 0, 1000 + 32000));
}

static void test_limitsRejected()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::TOO_BIG, r.add(1, 5, blobCrc, 513, 0, 4, blob, 160, 1000));
    TEST_ASSERT_EQUAL(Add::TOO_BIG, r.add(1, 5, blobCrc, 300, 0, 17, blob, 20, 1000));
    TEST_ASSERT_EQUAL(Add::INVALID, r.add(1, 5, blobCrc, 300, 2, 2, blob, 100, 1000)); // idx >= count
    TEST_ASSERT_EQUAL(Add::INVALID, r.add(1, 5, blobCrc, 300, 0, 2, blob, 0, 1000));   // fragLen 0
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_inOrder_completesWithMatchingBlob);
    RUN_TEST(test_outOfOrder_completes);
    RUN_TEST(test_duplicateFragment_idempotent);
    RUN_TEST(test_badCrc_rejectedAndReset);
    RUN_TEST(test_newKeyDropsOldTransfer);
    RUN_TEST(test_timeoutDropsStaleTransfer);
    RUN_TEST(test_limitsRejected);
    exit(UNITY_END());
}

void loop() {}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: suite `test_irrigation_fragment`
Expected: FAIL de compilação — header inexistente.

- [ ] **Step 3: Implementar**

`FragmentReassembler.h`:

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

// Remonta o blob de SET_CONFIG (spec §5.4/§5.5): uma transferência em curso por vez,
// fragmentos fora de ordem e duplicados tolerados, CRC32 validado no fechamento.
class FragmentReassembler
{
  public:
    static constexpr size_t MAX_CONFIG_LEN = 512;
    static constexpr uint8_t MAX_FRAGS = 16;
    static constexpr uint32_t TRANSFER_TIMEOUT_MS = 30000;

    enum class Add : uint8_t { ACCEPTED, COMPLETE, TOO_BIG, BAD_CRC, INVALID };

    Add add(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen, uint8_t fragIndex, uint8_t fragCount,
            const uint8_t *data, uint8_t fragLen, uint32_t nowMs);
    const uint8_t *blob() const { return buf; }
    uint16_t blobLen() const { return curTotalLen; }
    uint32_t epoch() const { return curEpoch; }
    void reset();

  private:
    bool sameTransfer(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen, uint8_t fragCount) const;
    bool active = false;
    uint32_t curSender = 0;
    uint32_t curEpoch = 0;
    uint32_t curCrc = 0;
    uint16_t curTotalLen = 0;
    uint8_t curFragCount = 0;
    uint16_t receivedMask = 0;
    uint32_t lastMs = 0;
    uint8_t buf[MAX_CONFIG_LEN];
};
```

`FragmentReassembler.cpp`:

```cpp
#include "FragmentReassembler.h"
#include "IrrigationProtocol.h"
#include <string.h>

void FragmentReassembler::reset()
{
    active = false;
    receivedMask = 0;
}

bool FragmentReassembler::sameTransfer(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen,
                                       uint8_t fragCount) const
{
    return curSender == sender && curEpoch == epoch && curCrc == crc && curTotalLen == totalLen &&
           curFragCount == fragCount;
}

FragmentReassembler::Add FragmentReassembler::add(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen,
                                                  uint8_t fragIndex, uint8_t fragCount, const uint8_t *data,
                                                  uint8_t fragLen, uint32_t nowMs)
{
    if (totalLen > MAX_CONFIG_LEN || fragCount > MAX_FRAGS)
        return Add::TOO_BIG;
    if (fragCount == 0 || fragIndex >= fragCount || fragLen == 0)
        return Add::INVALID;

    bool stale = active && (nowMs - lastMs) > TRANSFER_TIMEOUT_MS;
    if (!active || stale || !sameTransfer(sender, epoch, crc, totalLen, fragCount)) {
        active = true;
        curSender = sender;
        curEpoch = epoch;
        curCrc = crc;
        curTotalLen = totalLen;
        curFragCount = fragCount;
        receivedMask = 0;
    }
    lastMs = nowMs;

    uint16_t off = (uint16_t)fragIndex * IrrigationProto::FRAG_DATA_MAX;
    if (off + fragLen > curTotalLen)
        return Add::INVALID;
    memcpy(buf + off, data, fragLen);
    receivedMask |= (uint16_t)(1u << fragIndex);

    uint16_t full = (uint16_t)((1u << curFragCount) - 1);
    if (receivedMask != full)
        return Add::ACCEPTED;

    if (IrrigationProto::crc32(buf, curTotalLen) != curCrc) {
        reset();
        return Add::BAD_CRC;
    }
    return Add::COMPLETE;
}
```

Nota ao executor: o offset assume fragmentos de exatamente `FRAG_DATA_MAX` bytes exceto o último — o remetente (Task 4 e o futuro gateway) DEVE fragmentar assim. O teste usa 160+140 e cobre isso.

- [ ] **Step 4: Rodar e ver passar**

Run: suite `test_irrigation_fragment`
Expected: PASS (7 testes).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/FragmentReassembler.* test/test_irrigation_fragment/
git commit -m "feat(irrigation): add fragment reassembler with crc validation and timeout"
```

---

### Task 4: Wiring no módulo — SET/GET_CONFIG, epoch, modo seguro, boot heartbeat, `setNumValves`

**Files:**
- Modify: `src/modules/irrigation/ValveController.h` / `.cpp` (adicionar `setNumValves`)
- Modify: `src/modules/irrigation/IrrigationModule.h` / `.cpp`
- Test: `test/test_irrigation_valve/test_main.cpp` (estender com testes de `setNumValves`)

**Interfaces:**
- Consumes: `migrateIrrigationSettings` (Task 2), `FragmentReassembler` (Task 3), `encodeSetConfig`/`decodeSetConfig`/`crc32`/`FRAG_DATA_MAX`/reasons novos (Task 1); tudo da Fase 1.
- Produces: `void ValveController::setNumValves(uint8_t n, ...)` (assinatura abaixo); módulo com handlers `handleSetConfig`/`handleGetConfig`, membro `bool safeMode`, `FragmentReassembler reasm`, boot-heartbeat.

- [ ] **Step 1: Testes de `setNumValves` que falham**

Acrescentar a `test/test_irrigation_valve/test_main.cpp` (+ RUN_TEST):

```cpp
static void test_setNumValves_shrinkForceClosesRemoved()
{
    MockDriver d;
    ValveController vc(d, 4);
    vc.open(3, 60, 0, 0);
    vc.setNumValves(2);
    TEST_ASSERT_FALSE(vc.isOpen(3));
    TEST_ASSERT_EQUAL_INT(1, d.closes[3]); // removida foi fechada fisicamente
    TEST_ASSERT_EQUAL(ValveController::Result::INVALID_ID, vc.open(3, 60, 0, 0));
}

static void test_setNumValves_growPulsesNewClosed()
{
    MockDriver d;
    ValveController vc(d, 1);
    vc.setNumValves(3);
    // índices novos têm estado físico desconhecido: pulso de fechar em cada um
    TEST_ASSERT_EQUAL_INT(1, d.closes[1]);
    TEST_ASSERT_EQUAL_INT(1, d.closes[2]);
    TEST_ASSERT_EQUAL(ValveController::Result::OK, vc.open(2, 60, 0, 0));
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: suite `test_irrigation_valve`
Expected: FAIL de compilação — `setNumValves` inexistente.

- [ ] **Step 3: Implementar `setNumValves`**

`ValveController.h`, na seção pública:

```cpp
    // Reconfiguração em runtime (SET_CONFIG): encolher fecha fisicamente as
    // removidas; crescer pulsa fechar nas novas (estado físico desconhecido).
    void setNumValves(uint8_t n);
```

`ValveController.cpp`:

```cpp
void ValveController::setNumValves(uint8_t n)
{
    if (n > MAX_VALVES)
        n = MAX_VALVES;
    for (uint8_t i = n; i < numValves; i++) { // encolheu: fecha removidas
        driver.pulse(i, false);
        slots[i].open = false;
    }
    for (uint8_t i = numValves; i < n; i++) { // cresceu: novas em estado desconhecido
        driver.pulse(i, false);
        slots[i].open = false;
    }
    numValves = n;
}
```

- [ ] **Step 4: Rodar e ver passar**

Run: suite `test_irrigation_valve`
Expected: PASS (12 testes: 10 antigos + 2 novos).

- [ ] **Step 5: Commit parcial**

```bash
git add src/modules/irrigation/ValveController.* test/test_irrigation_valve/
git commit -m "feat(irrigation): allow runtime valve-count reconfiguration with safe pulses"
```

- [ ] **Step 6: Wiring do módulo**

`IrrigationModule.h` — acrescentar include, membros e métodos:

```cpp
#include "modules/irrigation/FragmentReassembler.h"
// ... na seção private, junto aos handlers:
    void handleSetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void applySettings(const IrrigationSettings &fresh);
// ... junto aos membros:
    FragmentReassembler reasm;
    bool safeMode = false;
    bool bootHeartbeatPending = true;
```

`IrrigationModule.cpp` — mudanças:

1. Construtor: detectar modo seguro (load falhou) e reportar. Trocar o início por:

```cpp
IrrigationModule::IrrigationModule()
    : SinglePortModule("irrigation", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Irrigation"),
      settings(loadIrrigationSettingsOrDefault()), valves(driver, settings.numValves), rateLimiter(settings.cmdRatePerMin)
{
    IrrigationSettings probe;
    safeMode = !loadIrrigationSettings(probe); // sem config persistida = modo seguro (spec §5.5)
    driver.configure(settings);
    valves.forceCloseAll();
    LOG_INFO("IrrigationModule role=%d valves=%d gateway=0x%08x epoch=%u safe=%d", settings.role, settings.numValves,
             settings.boundGateway, settings.configEpoch, (int)safeMode);
}
```

Nota ao executor: o construtor atual já chama `loadIrrigationSettingsOrDefault()`; a mudança real é a linha do `safeMode` e o log. Evitar o segundo load se preferir: mudar `loadIrrigationSettingsOrDefault` para setar um `static bool` de sucesso e ler daqui — qualquer das duas formas serve; a de cima é a mais simples e o custo (um read de 52 B no boot) é irrelevante.

2. `handleReceived`: adicionar os cases no switch:

```cpp
    case MSG_SET_CONFIG:
        handleSetConfig(mp, h);
        break;
    case MSG_GET_CONFIG:
        handleGetConfig(mp, h);
        break;
```

3. `handleCmdValvula`: logo após o rate-limit e o decode, antes de executar, bloquear abertura em modo seguro:

```cpp
    if (safeMode && cmd.action == 1) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_SAFE_MODE);
        return;
    }
```

4. Handlers novos (no fim do arquivo, antes de `runOnce`):

```cpp
void IrrigationModule::applySettings(const IrrigationSettings &fresh)
{
    // role e boundGateway são identidade/credencial: config remota não toca (§5.5/§6)
    uint8_t keepRole = settings.role;
    uint32_t keepGw = settings.boundGateway;
    settings = fresh;
    settings.role = keepRole;
    settings.boundGateway = keepGw;
    if (settings.pulseMs > 1000)
        settings.pulseMs = 1000;
    driver.configure(settings);
    valves.setNumValves(settings.numValves);
    rateLimiter = RateLimiter(settings.cmdRatePerMin);
}

void IrrigationModule::handleSetConfig(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        return;
    }
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: replayed config seq %u from 0x%08x", h.seq, mp.from);
        return;
    }
    if (!rateLimiter.allow(millis())) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_RATE_LIMIT);
        return;
    }

    SetConfig sc;
    if (!decodeSetConfig(mp.decoded.payload.bytes, mp.decoded.payload.size, sc)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        return;
    }

    auto r = reasm.add(mp.from, sc.epoch, sc.crc, sc.totalLen, sc.fragIndex, sc.fragCount, sc.frag, sc.fragLen, millis());
    switch (r) {
    case FragmentReassembler::Add::ACCEPTED:
        return; // fragmento intermediário: sem ACK (ACK = commit, §5.5)
    case FragmentReassembler::Add::TOO_BIG:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_CONFIG_TOO_BIG);
        return;
    case FragmentReassembler::Add::INVALID:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_FRAG_INVALID);
        return;
    case FragmentReassembler::Add::BAD_CRC:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_CRC);
        return;
    case FragmentReassembler::Add::COMPLETE:
        break;
    }

    IrrigationSettings fresh;
    if (!migrateIrrigationSettings(reasm.blob(), reasm.blobLen(), fresh)) {
        reasm.reset();
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        return;
    }
    uint32_t newEpoch = reasm.epoch();
    reasm.reset();

    applySettings(fresh);
    settings.configEpoch = newEpoch;
    if (!saveIrrigationSettings(settings)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_COMMIT_FAIL);
        return;
    }
    safeMode = false; // config persistida = saímos do modo seguro
    LOG_INFO("Irrigation: config applied epoch=%u from 0x%08x", newEpoch, mp.from);
    sendAck(mp.from, h.seq, ACK_OK, REASON_NONE); // ACK só após commit (§5.5)
}

void IrrigationModule::handleGetConfig(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        return;
    }
    if (!seqTable.checkAndUpdate(mp.from, h.seq))
        return;
    if (!rateLimiter.allow(millis()))
        return; // leitura: sem NACK, requisitante re-tenta

    // Resposta = o blob vigente no formato SET_CONFIG (leitura não altera epoch, §11.6)
    const uint8_t *blob = (const uint8_t *)&settings;
    uint16_t totalLen = sizeof(settings);
    uint32_t crc = crc32(blob, totalLen);
    uint8_t fragCount = (uint8_t)((totalLen + FRAG_DATA_MAX - 1) / FRAG_DATA_MAX);
    for (uint8_t i = 0; i < fragCount; i++) {
        SetConfig sc = {};
        sc.epoch = settings.configEpoch;
        sc.crc = crc;
        sc.totalLen = totalLen;
        sc.fragIndex = i;
        sc.fragCount = fragCount;
        uint16_t off = (uint16_t)i * FRAG_DATA_MAX;
        sc.fragLen = (uint8_t)((totalLen - off > FRAG_DATA_MAX) ? FRAG_DATA_MAX : totalLen - off);
        sc.frag = blob + off;

        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = mp.from;
        p->decoded.payload.size =
            (uint16_t)encodeSetConfig(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, sc);
        if (!p->decoded.payload.size) {
            packetPool.release(p);
            return;
        }
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    }
}
```

5. `sendAck` e `sendHeartbeat`: preencher epoch e flag de modo seguro:

```cpp
// em sendAck:
    ack.configEpoch = settings.configEpoch;
// em sendHeartbeat:
    hb.configEpoch = settings.configEpoch;
    hb.flags = safeMode ? HB_FLAG_SAFE_MODE : 0;
```

6. `runOnce`: boot heartbeat (anuncia estado pós-reset ~10 s após o boot, só ESTACAO):

```cpp
    // depois do bloco de lockout de bateria:
    if (bootHeartbeatPending && millis() > 10000) {
        bootHeartbeatPending = false;
        lastHeartbeatMs = millis();
        sendHeartbeat();
    }
```

- [ ] **Step 7: Compilar + suites de irrigação**

Run: suites `test_irrigation_valve`, `test_irrigation_config`, `test_irrigation_fragment`, `test_irrigation_protocol` (compilam todo o src).
Expected: todas PASSED, sem erro citando IrrigationModule.

- [ ] **Step 8: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.*
git commit -m "feat(irrigation): remote config with fragmented transfer, epoch and safe mode"
```

---

### Task 5: Contagem de suites + suite completa + roadmap

**Files:**
- Modify: `test/native-suite-count` (34 → 36)
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (Fase 2 → "concluída", com data)

**Interfaces:** consome tudo; nada novo.

- [ ] **Step 1: Atualizar `test/native-suite-count`**

Conteúdo do arquivo: `36`

- [ ] **Step 2: Suite completa**

```
MSYS_NO_PATHCONV=1 docker run --rm --name irrig-p2f -u 0 -e HOME=/root -v "$(pwd -W 2>/dev/null || pwd):/src:ro" -v pio-cache:/root -v fwtest-cache:/tmp/fw-test meshtastic-native-test timeout -k 60 3000 bash -c "cp -a /src/src/. /tmp/fw-test/src/ && cp -a /src/test/. /tmp/fw-test/test/ && cd /tmp/fw-test && platformio test -e coverage 2>&1 | tail -8"
```

Expected: `36` suites, sumário `N test cases: N succeeded`, 0 failed (~15-25 min).

- [ ] **Step 3: Atualizar roadmap**

Na tabela do roadmap, linha da Fase 2: trocar `futuro` por `concluída (2026-07-11, plano 2026-07-11-irrigacao-fase2-config.md)`. Registrar na mesma linha que a regra do maior epoch (lado gateway) foi movida para a Fase 4 (é comportamento do coordenador; a estação já reporta epoch em todo ACK/HB, que é o pré-requisito).

- [ ] **Step 4: Commit**

```bash
git add test/native-suite-count docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "test(irrigation): register phase 2 suites and update roadmap"
```

---

## Self-Review (executado na escrita do plano)

- **Cobertura §5**: §5.4 epoch persistido + reportado em ACK/HB + adotado no SET_CONFIG ✔ (reconciliação/regra do maior epoch = gateway, movida explicitamente p/ Fase 4 e registrada no roadmap — Task 5); §5.5 escrita staged ✔ (Fase 1) + CRC no transporte ✔ + ACK-só-após-commit ✔ + modo seguro ✔ + config de fábrica embutida = defaults compilados ✔; fragmentação ≤200 B ✔; §11.6 leitura não altera epoch ✔. Backup exportável (§5.5) = painel web, Fase 5. Requisito do usuário (polaridade configurável de entradas) entra no formato v2 ✔ (uso efetivo das entradas = Fases 4/6).
- **Placeholders**: nenhum TBD; duas "notas ao executor" apontam decisão explícita, não código faltante (uma corrige deliberadamente um erro plantado no teste do Step 1 da Task 2 — remover a expressão inválida e usar variável local).
- **Consistência de tipos**: `migrateIrrigationSettings(const uint8_t*, size_t, IrrigationSettings&)` idêntica nas Tasks 2 e 4; `FragmentReassembler::add(...)` idêntica nas Tasks 3 e 4; `Add::{ACCEPTED,COMPLETE,TOO_BIG,BAD_CRC,INVALID}` idem; `setNumValves(uint8_t)` idem Tasks 4 (header/cpp/testes); reasons 7–11 usados na Task 4 são os declarados na Task 1; `FRAG_DATA_MAX=160` consistente codec/reassembler/handlers.
