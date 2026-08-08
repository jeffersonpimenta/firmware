# Modo Remoto Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Botoeira (botão físico) no gateway ou em qualquer nó aciona (toggle-latch) uma saída em outro nó/gateway, com múltiplas botoeiras por saída e LED de feedback no nó da botoeira.

**Architecture:** Gateway-central. Associações vivem numa `RemoteButtonTable` (flash própria, como zones/mirror/groups). Botoeira = entrada digital com papel "botão" (borda+debounce). Estação envia `MSG_REMOTE_TRIGGER{inputIdx}` ao gateway; gateway resolve o toggle e aciona via `gwSendValveCmd` (herda fail-safe/interlock/grupo/roteamento local+remoto); gateway empurra `MSG_REMOTE_LED{ledStates}` na mudança de estado. LED FSM no nó da botoeira (pisca→fixo→apaga + estado compartilhado).

**Tech Stack:** C++17, Meshtastic firmware, PlatformIO, Unity (testes nativos), LittleFS/FSCom, frontend estático vanilla JS (`data/irrigacao/`).

## Global Constraints

- **Protocolo VERSION = 1** — nenhum bump. Só tipos de mensagem aditivos (nós antigos ignoram).
- **ABI settings bump v6→v7**, apêndice **no FIM** (offsets v6 intactos). `sizeof(IrrigationSettings)` de **180→184**. `StationEntry.blob` acompanha 180→184.
- **Aditividade total**: `Zone`/IZN2, MirrorMode, demais tabelas intactos.
- **Padrão de teste do repo**: unidades **puras** são native-tested (Unity); a **cola** do `IrrigationModule` só compila/linka no nativo (não é coberta por unidade) e é validada por **build ESP32 no CI + banca 2+ nós**.
- **Endpoints/portal/frontend = CI/banca-only** — `IrrigationWebEndpoints.cpp` já é excluído do build nativo (`variants/native/portduino.ini`); nunca compila no nativo.
- **Testes no Windows rodam em Docker** (ver `MEMORY.md` → windows-native-test-docker / native-test-docker-cp-workaround). Filtro por suíte: `pio test -e native --filter <suite>`.
- **Uma entrada digital tem papel exclusivo**: mirror **ou** sensor **ou** botoeira.
- Toggle-latch: **sem duração** por associação; fail-safe (teto de duração) herdado do caminho de zona.
- Fim de cada tarefa: `git add` + `git commit` (mensagem convencional, sem co-author de terceiros).

---

## File Structure

**Criar:**
- `src/modules/irrigation/RemoteButtonTable.h` / `.cpp` — tabela de associações (gateway).
- `src/modules/irrigation/RemoteButtonEdge.h` — detector borda+debounce (header-only, puro).
- `src/modules/irrigation/RemoteLedFsm.h` — FSM do LED (header-only, puro).
- `test/test_remote_button_table/test_main.cpp` — suíte da tabela.
- `test/test_remote_button/test_main.cpp` — suíte edge + FSM + toggle helper.

**Modificar:**
- `src/modules/irrigation/IrrigationProtocol.h` / `.cpp` — tipos 14/15 + codec.
- `src/modules/irrigation/IrrigationSettings.h` / `.cpp` — v7 (campos + migrate).
- `src/modules/irrigation/GatewayTables.h` — `StationEntry.blob` 180→184, `SERIALIZED_ENTRY` 215→219.
- `src/modules/irrigation/IrrigationGateway.h` — membro `remoteButtons`.
- `src/modules/irrigation/IrrigationWebApi.h` / `.cpp` — parse/build/validate remote (+dedup).
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — cola: dispatch, handlers, runOnce, persistência, LED push, comando.
- `src/modules/irrigation/IrrigationWebEndpoints.h` / `.cpp` — endpoints CI-only.
- `data/irrigacao/app.js` / `index.html` / `mock.js` — UI painel + mock.
- `test/native-suite-count` — 69 → 71.
- `test/test_irrigation_protocol/test_main.cpp`, `test/test_irrigation_webapi/test_main.cpp`, `test/test_service_backup/test_main.cpp` — casos novos.
- `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` — registrar a fase.

---

## Task 1: Protocolo — REMOTE_TRIGGER (14) + REMOTE_LED (15)

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h`
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp`
- Test: `test/test_irrigation_protocol/test_main.cpp`

**Interfaces:**
- Produces:
  - `struct RemoteTrigger { uint8_t inputIdx; };`
  - `struct RemoteLed { uint8_t ledStates; };`
  - `size_t encodeRemoteTrigger(uint8_t*, size_t, uint32_t seq, const RemoteTrigger&);`
  - `bool decodeRemoteTrigger(const uint8_t*, size_t, RemoteTrigger&);`
  - `size_t encodeRemoteLed(uint8_t*, size_t, uint32_t seq, const RemoteLed&);`
  - `bool decodeRemoteLed(const uint8_t*, size_t, RemoteLed&);`
  - `enum MsgType { … MSG_REMOTE_TRIGGER = 14, MSG_REMOTE_LED = 15 };`

- [ ] **Step 1: Escrever o teste que falha** — em `test/test_irrigation_protocol/test_main.cpp`, adicionar (e registrar com `RUN_TEST` no runner do arquivo):

```cpp
static void test_remoteTrigger_roundTrip()
{
    uint8_t buf[32];
    IrrigationProto::RemoteTrigger m{3};
    size_t n = IrrigationProto::encodeRemoteTrigger(buf, sizeof(buf), 0x1122, m);
    TEST_ASSERT_TRUE(n > 0);
    IrrigationProto::Header h;
    TEST_ASSERT_TRUE(IrrigationProto::decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(IrrigationProto::MSG_REMOTE_TRIGGER, h.type);
    TEST_ASSERT_EQUAL_UINT32(0x1122, h.seq);
    IrrigationProto::RemoteTrigger out{};
    TEST_ASSERT_TRUE(IrrigationProto::decodeRemoteTrigger(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(3, out.inputIdx);
    // buffer curto → 0
    TEST_ASSERT_EQUAL_UINT(0, IrrigationProto::encodeRemoteTrigger(buf, 2, 1, m));
}

static void test_remoteLed_roundTrip()
{
    uint8_t buf[32];
    IrrigationProto::RemoteLed m{0b10};
    size_t n = IrrigationProto::encodeRemoteLed(buf, sizeof(buf), 7, m);
    TEST_ASSERT_TRUE(n > 0);
    IrrigationProto::RemoteLed out{};
    TEST_ASSERT_TRUE(IrrigationProto::decodeRemoteLed(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(0b10, out.ledStates);
}
```

- [ ] **Step 2: Rodar o teste e confirmar a falha**

Run: `pio test -e native --filter test_irrigation_protocol`
Expected: FALHA de compilação — `encodeRemoteTrigger` não declarado.

- [ ] **Step 3: Implementar** — em `IrrigationProtocol.h`: no `enum MsgType`, após `MSG_CMD_MAINT = 13,` acrescentar `MSG_REMOTE_TRIGGER = 14,` e `MSG_REMOTE_LED = 15,`. Após `struct RemoteCmd {…};` acrescentar:

```cpp
struct RemoteTrigger {
    uint8_t inputIdx; // 0..3
};

struct RemoteLed {
    uint8_t ledStates; // bit s = slot de LED s ligado
};
```

Junto às demais declarações de encode/decode, acrescentar:

```cpp
size_t encodeRemoteTrigger(uint8_t *buf, size_t len, uint32_t seq, const RemoteTrigger &m);
bool decodeRemoteTrigger(const uint8_t *buf, size_t len, RemoteTrigger &out);
size_t encodeRemoteLed(uint8_t *buf, size_t len, uint32_t seq, const RemoteLed &m);
bool decodeRemoteLed(const uint8_t *buf, size_t len, RemoteLed &out);
```

Em `IrrigationProtocol.cpp` (após `decodeRemoteCmd`, seguindo o padrão `Writer`/`Reader`):

```cpp
size_t encodeRemoteTrigger(uint8_t *buf, size_t len, uint32_t seq, const RemoteTrigger &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_REMOTE_TRIGGER, seq);
    w.u8(m.inputIdx);
    return w.ok ? w.pos : 0;
}

bool decodeRemoteTrigger(const uint8_t *buf, size_t len, RemoteTrigger &out)
{
    Reader r = bodyReader(buf, len);
    out.inputIdx = r.u8();
    return r.ok;
}

size_t encodeRemoteLed(uint8_t *buf, size_t len, uint32_t seq, const RemoteLed &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_REMOTE_LED, seq);
    w.u8(m.ledStates);
    return w.ok ? w.pos : 0;
}

bool decodeRemoteLed(const uint8_t *buf, size_t len, RemoteLed &out)
{
    Reader r = bodyReader(buf, len);
    out.ledStates = r.u8();
    return r.ok;
}
```

- [ ] **Step 4: Rodar o teste e confirmar sucesso**

Run: `pio test -e native --filter test_irrigation_protocol`
Expected: PASS (todos, incluindo os 2 novos).

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationProtocol.cpp test/test_irrigation_protocol/test_main.cpp
git commit -m "feat(irrigation): protocolo REMOTE_TRIGGER(14)+REMOTE_LED(15)"
```

---

## Task 2: Settings ABI v6→v7 (pinos de LED + papéis de entrada)

**Files:**
- Modify: `src/modules/irrigation/IrrigationSettings.h`
- Modify: `src/modules/irrigation/IrrigationSettings.cpp`
- Modify: `src/modules/irrigation/GatewayTables.h`
- Test: `test/test_irrigation_config/test_main.cpp` (suíte que exercita migrate; se o nome divergir, usar a suíte que já testa `migrateIrrigationSettings` — confirmar com `grep -rl migrateIrrigationSettings test/`).

**Interfaces:**
- Produces:
  - Campos: `int8_t pinsRemoteLed[2]`, `uint8_t digitalInBtnMask`, `uint8_t digitalInLedIdx`; `version = 7`.
  - `static constexpr size_t IRRIGATION_SETTINGS_V6_SIZE = 180;`
  - Inline puros: `bool digitalInIsButton(const IrrigationSettings&, uint8_t i);`
    `uint8_t digitalInLedSlot(const IrrigationSettings&, uint8_t i);` (retorna 0/1 ou 3=nenhum)

- [ ] **Step 1: Escrever o teste que falha** — na suíte de migrate, adicionar:

```cpp
static void test_migrate_v6_to_v7()
{
    IrrigationSettings src; // v6 por construção? NÃO — após bump o default é v7.
    // Monta um blob v6 (180 B) sinteticamente: pega defaults, força version=6, trunca em 180.
    IrrigationSettings d;
    d.version = 6;
    uint8_t raw[180];
    memcpy(raw, &d, 180);
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, 180, out));
    TEST_ASSERT_EQUAL_UINT16(7, out.version);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinsRemoteLed[0]);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinsRemoteLed[1]);
    TEST_ASSERT_EQUAL_UINT8(0, out.digitalInBtnMask);
    TEST_ASSERT_EQUAL_UINT8(3, digitalInLedSlot(out, 0)); // 3 = nenhum por default
    TEST_ASSERT_FALSE(digitalInIsButton(out, 0));
}

static void test_settings_v7_size()
{
    TEST_ASSERT_EQUAL_UINT(184, sizeof(IrrigationSettings));
}
```

- [ ] **Step 2: Rodar e confirmar falha**

Run: `pio test -e native --filter test_irrigation_config`
Expected: FALHA — `sizeof` != 184 e/ou `digitalInLedSlot` indefinido.

- [ ] **Step 3: Implementar** — em `IrrigationSettings.h`:
  1. `uint16_t version = 7;` (era 6).
  2. Após `uint16_t vbatCriticaCentiV = 1180;` acrescentar (apêndice v7):

```cpp
    // v7 (Modo Remoto): pinos de LED dedicados + papéis de entrada digital.
    // Apêndice no FIM — offsets v6 preservados.
    int8_t pinsRemoteLed[2] = {-1, -1}; // 2 LEDs dedicados de feedback (§modo remoto)
    uint8_t digitalInBtnMask = 0;       // bit i = entrada digital i é botoeira
    uint8_t digitalInLedIdx = 0;        // 2 bits por entrada (0..3): slot de LED 0/1, 3 = nenhum
```

  3. Acrescentar constante e asserts (junto aos existentes):

```cpp
static constexpr size_t IRRIGATION_SETTINGS_V6_SIZE = 180;
static_assert(offsetof(IrrigationSettings, pinsRemoteLed) == 180, "ABI v7");
static_assert(offsetof(IrrigationSettings, digitalInBtnMask) == 182, "ABI v7");
static_assert(offsetof(IrrigationSettings, digitalInLedIdx) == 183, "ABI v7");
```

  4. Trocar `static_assert(sizeof(IrrigationSettings) == 180, …)` por `== 184`.
  5. Acrescentar os helpers puros (após o `struct`):

```cpp
inline bool digitalInIsButton(const IrrigationSettings &s, uint8_t i)
{
    return i < IrrigationSettings::MAX_DIGITAL_IN && ((s.digitalInBtnMask >> i) & 1u);
}
// Retorna 0 ou 1 (slot de LED) ou 3 (nenhum).
inline uint8_t digitalInLedSlot(const IrrigationSettings &s, uint8_t i)
{
    return (uint8_t)((s.digitalInLedIdx >> (2u * i)) & 0x3u);
}
```

  Em `IrrigationSettings.cpp` (`migrateIrrigationSettings`):
  - Trocar o ramo `if (version == 6)`: passa a ser `version == 7` exigindo `n == sizeof(IrrigationSettings)` (184).
  - Acrescentar novo ramo `version == 6` que aceita `n == IRRIGATION_SETTINGS_V6_SIZE` (180), copia o prefixo e `s.version = 7`:

```cpp
    if (version == 7) {
        if (n != sizeof(IrrigationSettings))
            return false;
        memcpy(&out, raw, sizeof(out));
        return true;
    }
    if (version == 6) {
        if (n != IRRIGATION_SETTINGS_V6_SIZE)
            return false;
        IrrigationSettings s; // defaults v7 (pinsRemoteLed=-1, masks=0)
        memcpy(&s, raw, IRRIGATION_SETTINGS_V6_SIZE); // v6 é prefixo do v7
        s.version = 7;
        out = s;
        return true;
    }
```

  - Nos ramos v5/v4/v3/v2/v1: trocar `s.version = 6;` por `s.version = 7;` (os defaults v7 já cobrem os campos novos).

  Em `GatewayTables.h`: `uint8_t blob[184] = {0};` (era 180) e comentário “config v7”; `static constexpr size_t SERIALIZED_ENTRY = 219;` (era 215) com o comentário aritmético atualizado (`blob(184)` ⇒ 219).

- [ ] **Step 4: Rodar e confirmar sucesso**

Run: `pio test -e native --filter test_irrigation_config`
Expected: PASS.

- [ ] **Step 5: Rodar as suítes que dependem do tamanho do blob** (regressão da ABI):

Run: `pio test -e native --filter test_irrigation_gwtables --filter test_irrigation_monitor`
Expected: PASS (o `static_assert` de `GatewayTables.cpp` `sizeof(IrrigationSettings) <= blob` compila com blob=184).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationSettings.h src/modules/irrigation/IrrigationSettings.cpp src/modules/irrigation/GatewayTables.h test/test_irrigation_config/test_main.cpp
git commit -m "feat(irrigation): settings ABI v7 (pinsRemoteLed[2]+btnMask+ledIdx, 180->184)"
```

---

## Task 3: RemoteButtonTable (tabela de associações do gateway)

**Files:**
- Create: `src/modules/irrigation/RemoteButtonTable.h`
- Create: `src/modules/irrigation/RemoteButtonTable.cpp`
- Create: `test/test_remote_button_table/test_main.cpp`
- Modify: `test/native-suite-count` (69 → 70; a segunda suíte vem na Task 4 → 71)

**Interfaces:**
- Produces:
  - `struct RemoteTriggerRef { uint32_t node; uint8_t inputIdx; uint8_t ledSlot; };` (`ledSlot`: 0/1 ou 255=nenhum)
  - `struct RemoteAssoc { uint8_t id; uint8_t enabled; uint8_t targetZoneId; RemoteTriggerRef triggers[4]; uint8_t triggerCount() const; };`
  - `class RemoteButtonTable` com `MAX=8`, `MAX_TRIGGERS=4`, `MAGIC=0x49524231`, e
    `upsert/removeById/byId/assocAt/count/findByTrigger/serialize/deserialize`.
  - `findByTrigger(uint32_t node, uint8_t inputIdx, const RemoteAssoc **out, size_t outCap) → size_t`.

- [ ] **Step 1: Escrever o teste que falha** — `test/test_remote_button_table/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/RemoteButtonTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static RemoteAssoc mkAssoc(uint8_t id, uint8_t zoneId, uint32_t n0, uint8_t in0)
{
    RemoteAssoc a;
    a.id = id;
    a.enabled = 1;
    a.targetZoneId = zoneId;
    a.triggers[0] = {n0, in0, 0};
    return a;
}

static void test_upsert_lookup_count()
{
    RemoteButtonTable t;
    TEST_ASSERT_TRUE(t.upsert(mkAssoc(1, 5, 0xAA, 2)));
    TEST_ASSERT_TRUE(t.upsert(mkAssoc(2, 6, 0xBB, 0)));
    TEST_ASSERT_EQUAL_UINT(2, t.count());
    RemoteAssoc a1b = mkAssoc(1, 9, 0xCC, 1); // mesmo id substitui
    TEST_ASSERT_TRUE(t.upsert(a1b));
    TEST_ASSERT_EQUAL_UINT(2, t.count());
    TEST_ASSERT_EQUAL_UINT8(9, t.byId(1)->targetZoneId);
    TEST_ASSERT_NULL(t.byId(99));
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_NULL(t.byId(1));
}

static void test_findByTrigger_multi()
{
    RemoteButtonTable t;
    RemoteAssoc a = mkAssoc(1, 5, 0xAA, 2);
    a.triggers[1] = {0xBB, 3, 1}; // segunda botoeira, outro nó
    t.upsert(a);
    t.upsert(mkAssoc(2, 5, 0xBB, 3)); // outra assoc, MESMO gatilho (0xBB,3) → mesma saída
    const RemoteAssoc *out[RemoteButtonTable::MAX];
    size_t k = t.findByTrigger(0xBB, 3, out, RemoteButtonTable::MAX);
    TEST_ASSERT_EQUAL_UINT(2, k); // ambas casam
    TEST_ASSERT_EQUAL_UINT(0, t.findByTrigger(0x99, 0, out, RemoteButtonTable::MAX));
    TEST_ASSERT_EQUAL_UINT8(2, t.byId(1)->triggerCount());
}

static void test_full_and_idzero_rejected()
{
    RemoteButtonTable t;
    for (uint8_t i = 1; i <= RemoteButtonTable::MAX; i++)
        TEST_ASSERT_TRUE(t.upsert(mkAssoc(i, i, i, 0)));
    TEST_ASSERT_FALSE(t.upsert(mkAssoc(200, 1, 1, 0)));
    TEST_ASSERT_FALSE(t.upsert(mkAssoc(0, 1, 1, 0)));
}

static void test_serialize_round_trip()
{
    RemoteButtonTable t;
    RemoteAssoc a = mkAssoc(1, 5, 0xAA, 2);
    a.triggers[1] = {0xBB, 3, 1};
    a.enabled = 0;
    t.upsert(a);
    uint8_t buf[512];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    RemoteButtonTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(1, t2.count());
    const RemoteAssoc *r = t2.byId(1);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8(0, r->enabled);
    TEST_ASSERT_EQUAL_HEX32(0xBB, r->triggers[1].node);
    TEST_ASSERT_EQUAL_UINT8(1, r->triggers[1].ledSlot);
    TEST_ASSERT_FALSE(t2.deserialize(buf, 3)); // curto → tabela vazia
    TEST_ASSERT_EQUAL_UINT(0, t2.count());
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_upsert_lookup_count);
    RUN_TEST(test_findByTrigger_multi);
    RUN_TEST(test_full_and_idzero_rejected);
    RUN_TEST(test_serialize_round_trip);
    return UNITY_END();
}
```

- [ ] **Step 2: Rodar e confirmar falha**

Run: `pio test -e native --filter test_remote_button_table`
Expected: FALHA — `RemoteButtonTable.h` inexistente.

- [ ] **Step 3: Implementar `RemoteButtonTable.h`:**

```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

struct RemoteTriggerRef {
    uint32_t node = 0;      // 0 = vazio
    uint8_t inputIdx = 0;   // 0..3
    uint8_t ledSlot = 255;  // 0/1 = slot de LED no nó do gatilho; 255 = nenhum
};

struct RemoteAssoc {
    uint8_t id = 0;          // 0 = slot vazio
    uint8_t enabled = 1;     // 0 = desativada (mantida na tabela)
    uint8_t targetZoneId = 0; // saída-alvo = zona do gateway
    static constexpr size_t MAX_TRIGGERS = 4;
    RemoteTriggerRef triggers[MAX_TRIGGERS];
    uint8_t triggerCount() const;
};

class RemoteButtonTable {
  public:
    static constexpr size_t MAX = 8;
    static constexpr uint32_t MAGIC = 0x49524231; // "IRB1"
    bool upsert(const RemoteAssoc &a);   // por id (1..255); false = cheia
    bool removeById(uint8_t id);
    const RemoteAssoc *byId(uint8_t id) const;
    const RemoteAssoc *assocAt(size_t index) const; // index-ésima ocupada; nullptr se >= count()
    size_t count() const;
    // Preenche out[] com associações cujo gatilho == (node,inputIdx); retorna quantas.
    size_t findByTrigger(uint32_t node, uint8_t inputIdx, const RemoteAssoc **out, size_t outCap) const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // falha => tabela vazia

  private:
    RemoteAssoc assocs[MAX];
};
```

  `RemoteButtonTable.cpp` (serialização no mesmo padrão de `GatewayTables.cpp`: `magic(4)+ver(1)+count(1)+entries`; `REMOTE_ENTRY = 3 + MAX_TRIGGERS*6 = 27`):

```cpp
#include "RemoteButtonTable.h"
#include <string.h>

static constexpr size_t REMOTE_ENTRY = 3 + RemoteAssoc::MAX_TRIGGERS * 6; // id,enabled,zone + 4×(node4+in1+led1)

uint8_t RemoteAssoc::triggerCount() const
{
    uint8_t c = 0;
    for (const auto &t : triggers)
        if (t.node != 0)
            c++;
    return c;
}

bool RemoteButtonTable::upsert(const RemoteAssoc &a)
{
    if (a.id == 0)
        return false;
    for (auto &s : assocs)
        if (s.id == a.id) { s = a; return true; }
    for (auto &s : assocs)
        if (s.id == 0) { s = a; return true; }
    return false;
}

bool RemoteButtonTable::removeById(uint8_t id)
{
    for (auto &s : assocs)
        if (s.id == id && id != 0) { s = RemoteAssoc{}; return true; }
    return false;
}

const RemoteAssoc *RemoteButtonTable::byId(uint8_t id) const
{
    if (id == 0) return nullptr;
    for (const auto &s : assocs)
        if (s.id == id) return &s;
    return nullptr;
}

const RemoteAssoc *RemoteButtonTable::assocAt(size_t index) const
{
    size_t seen = 0;
    for (const auto &s : assocs)
        if (s.id != 0) { if (seen == index) return &s; seen++; }
    return nullptr;
}

size_t RemoteButtonTable::count() const
{
    size_t c = 0;
    for (const auto &s : assocs)
        if (s.id != 0) c++;
    return c;
}

size_t RemoteButtonTable::findByTrigger(uint32_t node, uint8_t inputIdx, const RemoteAssoc **out, size_t outCap) const
{
    size_t k = 0;
    for (const auto &s : assocs) {
        if (s.id == 0) continue;
        for (const auto &t : s.triggers)
            if (t.node == node && t.node != 0 && t.inputIdx == inputIdx) {
                if (k < outCap) out[k] = &s;
                k++;
                break;
            }
    }
    return k;
}

size_t RemoteButtonTable::serialize(uint8_t *buf, size_t cap) const
{
    uint8_t cnt = (uint8_t)count();
    size_t need = 6 + (size_t)cnt * REMOTE_ENTRY;
    if (cap < need) return 0;
    uint32_t magic = MAGIC;
    memcpy(buf, &magic, 4);
    buf[4] = 1;
    buf[5] = cnt;
    size_t o = 6;
    for (const auto &s : assocs) {
        if (s.id == 0) continue;
        buf[o++] = s.id;
        buf[o++] = s.enabled;
        buf[o++] = s.targetZoneId;
        for (const auto &t : s.triggers) {
            memcpy(buf + o, &t.node, 4); o += 4;
            buf[o++] = t.inputIdx;
            buf[o++] = t.ledSlot;
        }
    }
    return need;
}

bool RemoteButtonTable::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : assocs) s = RemoteAssoc{};
    if (n < 6) return false;
    uint32_t m; memcpy(&m, buf, 4);
    if (m != MAGIC || buf[4] != 1) return false;
    uint8_t cnt = buf[5];
    if (cnt > MAX || n != 6 + (size_t)cnt * REMOTE_ENTRY) return false;
    size_t o = 6;
    for (uint8_t i = 0; i < cnt; i++) {
        RemoteAssoc a;
        a.id = buf[o++]; a.enabled = buf[o++]; a.targetZoneId = buf[o++];
        for (auto &t : a.triggers) {
            memcpy(&t.node, buf + o, 4); o += 4;
            t.inputIdx = buf[o++]; t.ledSlot = buf[o++];
        }
        assocs[i] = a;
    }
    return true;
}
```

- [ ] **Step 4: Ajustar contagem de suítes** — em `test/native-suite-count`, trocar `69` por `70`.

- [ ] **Step 5: Rodar e confirmar sucesso**

Run: `pio test -e native --filter test_remote_button_table`
Expected: PASS (4 testes).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/RemoteButtonTable.h src/modules/irrigation/RemoteButtonTable.cpp test/test_remote_button_table/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): RemoteButtonTable (associações botoeira->saída, native-tested)"
```

---

## Task 4: RemoteButtonEdge + RemoteLedFsm + toggle helper (unidades puras)

**Files:**
- Create: `src/modules/irrigation/RemoteButtonEdge.h`
- Create: `src/modules/irrigation/RemoteLedFsm.h`
- Create: `test/test_remote_button/test_main.cpp`
- Modify: `test/native-suite-count` (70 → 71)

**Interfaces:**
- Produces:
  - `class RemoteButtonEdge { bool update(bool raw, uint32_t nowMs); };` — true 1× na borda de subida pós-debounce.
  - `class RemoteLedFsm { void onPress(uint32_t); void onLedState(bool on, uint32_t); bool ledOn(uint32_t); State state(uint32_t); };`
  - `inline uint8_t remoteToggleAction(bool currentlyOn)` — 1=abrir se desligado, 0=fechar se ligado (em `RemoteButtonEdge.h`).

- [ ] **Step 1: Escrever o teste que falha** — `test/test_remote_button/test_main.cpp`:

```cpp
#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/RemoteButtonEdge.h"
#include "modules/irrigation/RemoteLedFsm.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_edge_debounce_risingOnce()
{
    RemoteButtonEdge e;
    TEST_ASSERT_FALSE(e.update(false, 0));
    TEST_ASSERT_FALSE(e.update(true, 10));   // subiu mas dentro do debounce
    TEST_ASSERT_FALSE(e.update(true, 40));   // ainda instável (< 50ms)
    TEST_ASSERT_TRUE(e.update(true, 70));    // estável → 1 borda
    TEST_ASSERT_FALSE(e.update(true, 200));  // segurando: sem repetir
    TEST_ASSERT_FALSE(e.update(false, 260)); // soltou (debounce)
    TEST_ASSERT_FALSE(e.update(false, 320)); // estável baixo, rearma
    TEST_ASSERT_TRUE(e.update(true, 400));   // nova subida (após rearmar)
}

static void test_led_fsm()
{
    RemoteLedFsm f;
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::OFF, f.state(0));
    f.onPress(1000);
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::BLINK, f.state(1000));
    // pisca: alterna com o tempo
    bool a = f.ledOn(1000), b = f.ledOn(1000 + RemoteLedFsm::BLINK_PERIOD_MS);
    TEST_ASSERT_NOT_EQUAL(a, b);
    f.onLedState(true, 1200); // push do gateway = ligado
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::SOLID, f.state(1200));
    TEST_ASSERT_TRUE(f.ledOn(9999));
    f.onLedState(false, 1300); // saída desligou
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::OFF, f.state(1300));
    // timeout: press sem push → apaga
    f.onPress(2000);
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::BLINK, f.state(2000));
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::OFF, f.state(2000 + RemoteLedFsm::BLINK_TIMEOUT_MS + 1));
}

static void test_toggle_action()
{
    TEST_ASSERT_EQUAL_UINT8(1, remoteToggleAction(false)); // desligado → abrir
    TEST_ASSERT_EQUAL_UINT8(0, remoteToggleAction(true));  // ligado → fechar
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_edge_debounce_risingOnce);
    RUN_TEST(test_led_fsm);
    RUN_TEST(test_toggle_action);
    return UNITY_END();
}
```

- [ ] **Step 2: Rodar e confirmar falha**

Run: `pio test -e native --filter test_remote_button`
Expected: FALHA — headers inexistentes.

- [ ] **Step 3: Implementar `RemoteButtonEdge.h`:**

```cpp
#pragma once
#include <stdint.h>

// Detector de borda de subida com debounce, para botoeiras (entradas digitais).
// raw = nível lógico JÁ com polaridade aplicada (true = pressionado). update() retorna
// true UMA vez por borda de subida confirmada; rearma só após ler estável em baixo.
class RemoteButtonEdge {
  public:
    static constexpr uint32_t DEBOUNCE_MS = 50;
    bool update(bool raw, uint32_t nowMs)
    {
        if (raw != lastRaw) { lastRaw = raw; sinceMs = nowMs; }
        if ((uint32_t)(nowMs - sinceMs) >= DEBOUNCE_MS && raw != stable) {
            stable = raw;
            if (stable) { // borda de subida estável
                if (armed) { armed = false; return true; }
            } else {
                armed = true; // soltou → rearma
            }
        }
        return false;
    }

  private:
    bool stable = false;
    bool lastRaw = false;
    uint32_t sinceMs = 0;
    bool armed = true;
};

// Resolução do toggle: dada a saída atual, devolve a ação (1 = abrir, 0 = fechar).
inline uint8_t remoteToggleAction(bool currentlyOn) { return currentlyOn ? 0u : 1u; }
```

  `RemoteLedFsm.h`:

```cpp
#pragma once
#include <stdint.h>

// FSM do LED de feedback da botoeira (no nó da botoeira). Uma instância por slot de LED.
// IDLE/OFF → (press) BLINK → (push ligado) SOLID | (push desligado / timeout) OFF.
class RemoteLedFsm {
  public:
    enum class State : uint8_t { OFF, BLINK, SOLID };
    static constexpr uint32_t BLINK_TIMEOUT_MS = 5000;
    static constexpr uint32_t BLINK_PERIOD_MS = 250;

    void onPress(uint32_t nowMs) { st = State::BLINK; pressMs = nowMs; }
    void onLedState(bool on, uint32_t) { st = on ? State::SOLID : State::OFF; }

    State state(uint32_t nowMs)
    {
        if (st == State::BLINK && (uint32_t)(nowMs - pressMs) > BLINK_TIMEOUT_MS)
            st = State::OFF;
        return st;
    }

    bool ledOn(uint32_t nowMs)
    {
        switch (state(nowMs)) {
        case State::SOLID: return true;
        case State::BLINK: return ((nowMs / BLINK_PERIOD_MS) & 1u) != 0;
        default: return false;
        }
    }

  private:
    State st = State::OFF;
    uint32_t pressMs = 0;
};
```

- [ ] **Step 4: Ajustar contagem de suítes** — `test/native-suite-count`: `70` → `71`.

- [ ] **Step 5: Rodar e confirmar sucesso**

Run: `pio test -e native --filter test_remote_button`
Expected: PASS (3 testes).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/RemoteButtonEdge.h src/modules/irrigation/RemoteLedFsm.h test/test_remote_button/test_main.cpp test/native-suite-count
git commit -m "feat(irrigation): RemoteButtonEdge (borda+debounce) + RemoteLedFsm + toggle helper"
```

---

## Task 5: IrrigationWebApi — parse/build/validate + dedup por saída

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `RemoteButtonTable`, `RemoteAssoc`, `ZoneTable` (Task 3, existente).
- Produces (namespace `IrrigationWeb`):
  - `struct RemoteUpsertReq { uint8_t id; uint8_t enabled; uint8_t targetZoneId; RemoteTriggerRef triggers[4]; uint8_t triggerCount; };`
  - `bool parseRemoteUpsert(const char *json, size_t n, RemoteUpsertReq &out);`
  - `bool parseRemoteDelete(const char *json, size_t n, uint8_t &idOut);`
  - `bool parseRemoteCommand(const char *json, size_t n, uint8_t &targetZoneIdOut);`
  - `bool validateRemoteTriggers(const RemoteUpsertReq &u);` (≥1 gatilho, sem nó duplicado, inputIdx<4, ledSlot∈{0,1,255}, targetZoneId≠0)
  - `size_t buildRemote(char *buf, size_t cap, const RemoteButtonTable &t, const ZoneTable &zones);`
  - `size_t buildRemoteStatus(char *buf, size_t cap, const RemoteButtonTable &t, const ZoneTable &zones, const bool *zoneOpenById /*index 0..255*/);` — **dedup por targetZoneId**.

- [ ] **Step 1: Escrever o teste que falha** — em `test/test_irrigation_webapi/test_main.cpp` acrescentar e registrar:

```cpp
static void test_parseRemoteUpsert_and_validate()
{
    const char *j = "{\"id\":0,\"enabled\":true,\"targetZoneId\":5,"
                    "\"triggers\":[{\"node\":170,\"inputIdx\":2,\"ledSlot\":0},"
                    "{\"node\":187,\"inputIdx\":3,\"ledSlot\":255}]}";
    IrrigationWeb::RemoteUpsertReq u{};
    TEST_ASSERT_TRUE(IrrigationWeb::parseRemoteUpsert(j, strlen(j), u));
    TEST_ASSERT_EQUAL_UINT8(5, u.targetZoneId);
    TEST_ASSERT_EQUAL_UINT8(2, u.triggerCount);
    TEST_ASSERT_EQUAL_HEX32(170, u.triggers[0].node);
    TEST_ASSERT_EQUAL_UINT8(0, u.triggers[0].ledSlot);
    TEST_ASSERT_TRUE(IrrigationWeb::validateRemoteTriggers(u));

    IrrigationWeb::RemoteUpsertReq dup = u; // mesmo nó nos 2 gatilhos → inválido
    dup.triggers[1].node = 170;
    TEST_ASSERT_FALSE(IrrigationWeb::validateRemoteTriggers(dup));

    IrrigationWeb::RemoteUpsertReq empty{};
    empty.targetZoneId = 5;
    TEST_ASSERT_FALSE(IrrigationWeb::validateRemoteTriggers(empty)); // sem gatilho
}

static void test_buildRemoteStatus_dedupByOutput()
{
    ZoneTable z;
    Zone zz; zz.id = 5; snprintf(zz.name, sizeof(zz.name), "Bomba"); zz.node = 170;
    z.upsert(zz);
    RemoteButtonTable t;
    RemoteAssoc a1; a1.id = 1; a1.targetZoneId = 5; a1.triggers[0] = {170, 2, 0}; t.upsert(a1);
    RemoteAssoc a2; a2.id = 2; a2.targetZoneId = 5; a2.triggers[0] = {187, 3, 1}; t.upsert(a2);
    bool open[256] = {false}; open[5] = true;
    char buf[1024];
    size_t n = IrrigationWeb::buildRemoteStatus(buf, sizeof(buf), t, z, open);
    TEST_ASSERT_TRUE(n > 0);
    // saída 5 aparece UMA vez apesar de 2 associações
    const char *first = strstr(buf, "\"targetZoneId\":5");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(strstr(first + 1, "\"targetZoneId\":5"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"on\":true"));
}
```

- [ ] **Step 2: Rodar e confirmar falha**

Run: `pio test -e native --filter test_irrigation_webapi`
Expected: FALHA — símbolos indefinidos.

- [ ] **Step 3: Implementar** — em `IrrigationWebApi.h` incluir `#include "modules/irrigation/RemoteButtonTable.h"` e declarar os tipos/funções da seção **Interfaces**. Em `IrrigationWebApi.cpp`:
  - `parseRemoteDelete`/`parseRemoteCommand`: usar `JsonReader` plano (`readUInt`) — espelhar `parseMirrorMapping`/`parseGroupDelete` existentes.
  - `parseRemoteUpsert`: array aninhado `triggers[]` — **espelhar `parseGroupUpsert`** (mesmo scanner de array de objetos usado para as zonas do grupo). Para cada objeto, ler `node`/`inputIdx`/`ledSlot` (default `ledSlot=255`). Preencher `triggers[]` até 4 e `triggerCount`.
  - `validateRemoteTriggers`: `targetZoneId != 0`, `triggerCount >= 1`, para cada gatilho `inputIdx < 4`, `ledSlot ∈ {0,1,255}`, e **sem nó repetido** (laço O(n²) sobre ≤4).
  - `buildRemote`: array JSON de associações (`id`, `enabled`, `targetZoneId`, nome da zona via `zones.byId`, `triggers[]`). Espelhar `buildMirror`/`buildGroups` para o estilo do writer.
  - `buildRemoteStatus`: iterar por **zonas-alvo distintas** (dedup): para cada `targetZoneId` presente em alguma associação, emitir um objeto `{targetZoneId, name, on: zoneOpenById[id], triggers:[...combinado...]}`. Usar um `bool seen[256]` local para deduplicar.

- [ ] **Step 4: Rodar e confirmar sucesso**

Run: `pio test -e native --filter test_irrigation_webapi`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): webapi remote parse/build/validate + dedup por saída"
```

---

## Task 6: Cola do gateway — tabela, persistência, trigger handler, botoeira local, toggle+drive, LED push

**Files:**
- Modify: `src/modules/irrigation/IrrigationGateway.h` (membro `RemoteButtonTable remoteButtons;`)
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarações)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (implementação)

**Interfaces:**
- Consumes: `RemoteButtonTable`, `RemoteButtonEdge`, `remoteToggleAction`, `encodeRemoteLed`, `decodeRemoteTrigger`, `gwSendValveCmd`, `senderAuthorizedBy`, `seqTable`.
- Produces (métodos privados no módulo): `handleRemoteTrigger`, `gwFireRemote(uint32_t node, uint8_t inputIdx)`, `gwPushRemoteLed(uint8_t targetZoneId)`, `saveRemoteButtons`/`loadRemoteButtons`, `gwApplyRemoteUpsert`/`gwApplyRemoteDelete`, `gwRunRemoteCommand(uint8_t targetZoneId)`.

> **Nota de cobertura:** esta tarefa é **cola do módulo** — compila/linka no nativo mas não tem teste de unidade (padrão das fases anteriores). A verificação é **build nativo + build ESP32 (CI) + banca**. Os passos abaixo não têm ciclo TDD; o "teste" é o build.

- [ ] **Step 1: Agregado** — em `IrrigationGateway.h`, adicionar `#include "modules/irrigation/RemoteButtonTable.h"` e o membro `RemoteButtonTable remoteButtons;`.

- [ ] **Step 2: Persistência** — em `IrrigationModule.cpp`, junto a `GW_MIRROR_PATH` (:39), declarar:

```cpp
static const char *GW_REMOTE_PATH = "/prefs/irrigation-remote.dat";
static const char *GW_REMOTE_TMP = "/prefs/irrigation-remote.tmp";
```

  Adicionar `saveRemoteButtons()`/`loadRemoteButtons()` espelhando o par mirror (`:1948`/`:1979` usam `stagedRead`/`stagedWrite`): buffer `uint8_t buf[6 + RemoteButtonTable::MAX*27]`, `gateway.remoteButtons.serialize/deserialize`. Chamar `loadRemoteButtons()` onde o estado do gateway é carregado (junto de `deserialize` do mirror, ~`:1949`) e `saveRemoteButtons()` em `saveGatewayState()`.

- [ ] **Step 3: Dispatch** — em `handleReceived` (switch de `h.type`, após `case MSG_REMOTE_CMD:` :387), adicionar:

```cpp
    case MSG_REMOTE_TRIGGER:
        if (settings.role == (uint8_t)IrrigationRole::GATEWAY)
            handleRemoteTrigger(mp, h);
        break;
```

  (O `case MSG_REMOTE_LED` é tratado na Task 7, no lado da estação.)

- [ ] **Step 4: `handleRemoteTrigger`** — espelha `handleRemoteCmd` (:3620): anti-replay `seqTable.checkAndUpdate`, `senderAuthorizedBy(h.flags, mp.from, settings.boundGateway)`, rate-limit. Decodifica `RemoteTrigger`, chama `gwFireRemote(mp.from, rt.inputIdx)`. **Fire-and-forget: sem ACK** (o LED-push é o feedback).

```cpp
void IrrigationModule::handleRemoteTrigger(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) return;
    if (!senderAuthorizedBy(h.flags, mp.from, settings.boundGateway)) return;
    if (!rateLimiter.allow(millis())) return;
    IrrigationProto::RemoteTrigger rt;
    if (!decodeRemoteTrigger(mp.decoded.payload.bytes, mp.decoded.payload.size, rt)) return;
    gwFireRemote(mp.from, rt.inputIdx);
}
```

- [ ] **Step 5: `gwFireRemote`** — resolve associações e toggla a saída via `gwSendValveCmd`, depois empurra o LED:

```cpp
void IrrigationModule::gwFireRemote(uint32_t node, uint8_t inputIdx)
{
    const RemoteAssoc *hits[RemoteButtonTable::MAX];
    size_t k = gateway.remoteButtons.findByTrigger(node, inputIdx, hits, RemoteButtonTable::MAX);
    for (size_t i = 0; i < k; i++) {
        const RemoteAssoc *a = hits[i];
        if (!a->enabled) continue;
        const Zone *z = gateway.zones.byId(a->targetZoneId);
        if (!z) continue;
        bool on = gwZoneIsOpen(a->targetZoneId); // estado autoritativo conhecido
        uint8_t action = remoteToggleAction(on);
        if (action == 1) gwRunCommandOpen(a->targetZoneId);   // reusa caminho manual (grupo/rota/fail-safe)
        else             gwRunCommandClose(a->targetZoneId);
        gwPushRemoteLed(a->targetZoneId);
    }
}
```

  **`gwZoneIsOpen(zoneId)`**: helper que devolve o estado conhecido — reusar o mesmo estado que `buildRemoteStatus`/painel usa para "ligada/desligada" (o módulo já rastreia abertura de zona para o overview/scheduler; usar a mesma fonte). **`gwRunCommandOpen/Close`**: usar os caminhos manuais já existentes (`portalRunNetCommand` ou o bloco manual `:3132-3143` que chama `routeZoneToGroup`+`gwSendValveCmd`) — extrair um helper se necessário, sem duplicar a coreografia de grupo. Duração de open = `z->padraoMin*60` clampada (mesma regra do comando manual).

- [ ] **Step 6: Botoeira LOCAL do gateway** — em `runOnce`, no bloco do gateway logo após a leitura do mirror (`:3441-3476`), acrescentar a leitura das entradas-botão locais com `RemoteButtonEdge` (um array `_gwBtnEdge[MAX_DIGITAL_IN]` membro do módulo):

```cpp
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
        if (settings.pinsDigitalIn[i] < 0 || !digitalInIsButton(settings, i)) continue;
#ifndef ARCH_PORTDUINO
        bool raw = (digitalRead(settings.pinsDigitalIn[i]) == HIGH);
#else
        bool raw = false;
#endif
        bool activeLow = (settings.digitalInActiveLow >> i) & 1;
        bool pressed = activeLow ? !raw : raw;
        if (_btnEdge[i].update(pressed, millis())) {
            uint8_t slot = digitalInLedSlot(settings, i); // pisca o LED local (igual à estação)
            if (slot <= 1) _remoteLed[slot].onPress(millis());
            gwFireRemote(nodeDB->getNodeNum(), i);
        }
    }
```

  (Botoeira local não conflita com mirror: `digitalInIsButton` marca papel exclusivo; a UI garante que uma entrada não seja mirror e botão ao mesmo tempo. O gateway só entra neste laço para entradas marcadas botão nos SEUS settings — ver Task 6 Step 8, que grava `digitalInBtnMask/LedIdx` locais para gatilhos self.)

- [ ] **Step 7: `gwPushRemoteLed`** — recomputa e envia `REMOTE_LED` **na mudança** aos nós-gatilho da(s) associação(ões) daquela saída. Para cada nó-gatilho distinto, montar o bitmap de slots (bit `ledSlot` = 1 se a saída-alvo do gatilho está ligada). Só envia se o bitmap mudou desde o último envio (cache `_lastLedStates[node]`). Se o nó-gatilho for o próprio gateway, aplicar aos LEDs locais (`pinsRemoteLed`) via `RemoteLedFsm` local em vez de rádio.

```cpp
void IrrigationModule::gwPushRemoteLed(uint8_t targetZoneId)
{
    // Reúne, por nó-gatilho, o estado de LED consolidado de TODAS as associações
    // (um nó pode ter gatilhos p/ várias saídas em slots diferentes).
    struct NodeLed { uint32_t node; uint8_t states; };
    NodeLed acc[RemoteButtonTable::MAX * RemoteAssoc::MAX_TRIGGERS];
    size_t na = 0;
    for (size_t ai = 0; ai < RemoteButtonTable::MAX; ai++) {
        const RemoteAssoc *a = gateway.remoteButtons.assocAt(ai);
        if (!a) break;
        bool on = gwZoneIsOpen(a->targetZoneId);
        for (const auto &t : a->triggers) {
            if (t.node == 0 || t.ledSlot > 1) continue;
            size_t j = 0; for (; j < na; j++) if (acc[j].node == t.node) break;
            if (j == na) { acc[na++] = {t.node, 0}; }
            if (on) acc[j].states |= (uint8_t)(1u << t.ledSlot);
        }
    }
    for (size_t j = 0; j < na; j++) {
        if (_lastLedStates_lookup(acc[j].node) == acc[j].states) continue; // change-driven
        _lastLedStates_set(acc[j].node, acc[j].states);
        if (acc[j].node == nodeDB->getNodeNum()) { applyLocalRemoteLeds(acc[j].states); continue; }
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = acc[j].node;
        IrrigationProto::RemoteLed rl{acc[j].states};
        p->decoded.payload.size = (uint16_t)IrrigationProto::encodeRemoteLed(
            p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, rl);
        if (!p->decoded.payload.size) { packetPool.release(p); continue; }
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    }
}
```

  `_lastLedStates_*` = cache pequeno (mapa node→states, ~16 entradas). `applyLocalRemoteLeds(states)` = dirige `pinsRemoteLed[slot]` do gateway via `RemoteLedFsm` local (mesma instância usada na Task 7 para estação — o gateway também tem LEDs). **Chamar `gwPushRemoteLed`** também quando uma saída muda por OUTRA origem (scheduler/manual/fail-safe): no ponto onde o estado de abertura de zona muda (após ACK em `confirmCommand` / no fechamento por fail-safe), chamar `gwPushRemoteLed(zoneId)` para cobrir "estado compartilhado".

- [ ] **Step 8: `gwApplyRemoteUpsert`/`gwApplyRemoteDelete`/`gwRunRemoteCommand`** — usados pelos endpoints (Task 8):

```cpp
bool IrrigationModule::gwApplyRemoteUpsert(const IrrigationWeb::RemoteUpsertReq &u)
{
    if (!IrrigationWeb::validateRemoteTriggers(u)) return false;
    RemoteAssoc a;
    a.id = u.id ? u.id : gwAllocRemoteId(); // id=0 → aloca livre
    a.enabled = u.enabled; a.targetZoneId = u.targetZoneId;
    for (size_t i = 0; i < RemoteAssoc::MAX_TRIGGERS; i++) a.triggers[i] = u.triggers[i];
    if (!gateway.remoteButtons.upsert(a)) return false;
    saveRemoteButtons();
    gwPushStationBtnConfig(a); // re-push SET_CONFIG p/ estações-gatilho (Task 7 depende)
    gwPushRemoteLed(a.targetZoneId);
    return true;
}
```

  `gwApplyRemoteDelete(id)`: `removeById`+`saveRemoteButtons`+re-push config das estações afetadas. `gwRunRemoteCommand(targetZoneId)`: toggle manual pela UI (botão "Acionar") — mesma lógica de toggle de `gwFireRemote` para aquela saída. `gwAllocRemoteId()`: menor id 1..MAX livre.

  **`gwPushStationBtnConfig(assoc)`**: para cada nó-gatilho:
  - se for **estação**: atualizar o blob de config daquela estação (`StationRegistry`) marcando `digitalInBtnMask` bit `inputIdx` e `digitalInLedIdx` slot (2 bits em `2*inputIdx`), bump epoch, e re-`SET_CONFIG` (mesmo caminho de `gwApplyStationConfig` da fase 9). **Sem isto a estação não gera trigger nem pisca.**
  - se for o **próprio gateway** (`node == nodeDB->getNodeNum()`): marcar os mesmos bits nos **settings locais** (`settings.digitalInBtnMask |= 1<<inputIdx`; `settings.digitalInLedIdx` slot em `2*inputIdx`) e `saveIrrigationSettings(settings)` — o laço de leitura local (Step 6) gateia em `digitalInIsButton(settings,i)`, então sem isto a botoeira local nunca é lida.
  - **Ao remover/editar** uma associação, limpar os bits que deixaram de ter gatilho (recomputar a máscara a partir da tabela inteira em vez de só setar) para não deixar entrada marcada botão órfã.

- [ ] **Step 9: Declarações** — adicionar todos os métodos acima em `IrrigationModule.h` (privados) e os membros **compartilhados** (papéis são exclusivos em runtime, então gateway e estação reusam os mesmos): `RemoteButtonEdge _btnEdge[IrrigationSettings::MAX_DIGITAL_IN];`, cache `_lastLedStates` (node→states, ~16), e `RemoteLedFsm _remoteLed[2];`. A Task 7 reusa `_btnEdge` e `_remoteLed` — não os redeclara.

- [ ] **Step 10: Build nativo** (verificação da cola):

Run: `pio run -e native` (ou o script Docker native `bin/…` — ver MEMORY.md)
Expected: SUCCESS (`IrrigationModule.cpp` compila e linka).

- [ ] **Step 11: Commit**

```bash
git add src/modules/irrigation/IrrigationGateway.h src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): cola gateway do modo remoto (trigger/toggle/LED push/persistência)"
```

---

## Task 7: Cola da estação — borda→trigger, REMOTE_LED intake, FSM→pino

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h`
- Modify: `src/modules/irrigation/IrrigationModule.cpp`

**Interfaces:**
- Consumes: `RemoteButtonEdge`, `RemoteLedFsm`, `digitalInIsButton`, `digitalInLedSlot`, `encodeRemoteTrigger`, `decodeRemoteLed`.
- Produces: `handleRemoteLed`, bloco de leitura de botoeira no `runOnce` da estação, drive de `pinsRemoteLed`.

> **Cola do módulo** — sem TDD; verificação = build nativo + banca.

- [ ] **Step 1: Membros** — reusar os membros **já declarados na Task 6**: `_btnEdge[IrrigationSettings::MAX_DIGITAL_IN]` e `_remoteLed[2]`. Nenhuma nova declaração (papéis exclusivos em runtime ⇒ compartilham as mesmas instâncias).

- [ ] **Step 2: Leitura de botoeira (estação)** — no `runOnce`, no ramo do papel ESTAÇÃO (onde os drivers locais fazem tick), acrescentar leitura idêntica à do gateway (Task 6 Step 6), mas ao detectar borda: **pisca o LED** e **envia `REMOTE_TRIGGER` ao gateway**:

```cpp
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
        if (settings.pinsDigitalIn[i] < 0 || !digitalInIsButton(settings, i)) continue;
#ifndef ARCH_PORTDUINO
        bool raw = (digitalRead(settings.pinsDigitalIn[i]) == HIGH);
#else
        bool raw = false;
#endif
        bool activeLow = (settings.digitalInActiveLow >> i) & 1;
        bool pressed = activeLow ? !raw : raw;
        if (_btnEdge[i].update(pressed, millis())) {
            uint8_t slot = digitalInLedSlot(settings, i);
            if (slot <= 1) _remoteLed[slot].onPress(millis());
            if (settings.boundGateway != 0) {
                meshtastic_MeshPacket *p = allocDataPacket();
                p->to = settings.boundGateway;
                IrrigationProto::RemoteTrigger rt{i};
                p->decoded.payload.size = (uint16_t)IrrigationProto::encodeRemoteTrigger(
                    p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, rt);
                if (p->decoded.payload.size) service->sendToMesh(p, RX_SRC_LOCAL, false);
                else packetPool.release(p);
            }
        }
    }
```

- [ ] **Step 3: Drive físico dos LEDs (estação e gateway)** — no `runOnce` (todos os papéis, junto do tick do `pinLed`), para cada slot 0/1 com `settings.pinsRemoteLed[slot] >= 0`, escrever `digitalWrite(settings.pinsRemoteLed[slot], _remoteLed[slot].ledOn(millis()))`. Guardar `#ifndef ARCH_PORTDUINO`.

- [ ] **Step 4: Intake `REMOTE_LED`** — no dispatch (`handleReceived`), acrescentar:

```cpp
    case MSG_REMOTE_LED:
        handleRemoteLed(mp, h);
        break;
```

  e o handler (aplica a todos os slots; idempotente, sem anti-replay estrito):

```cpp
void IrrigationModule::handleRemoteLed(const meshtastic_MeshPacket &mp, const Header &)
{
    // Só aceita do gateway vinculado (posse-da-PSK já garante canal; checagem leve).
    if (settings.boundGateway != 0 && mp.from != settings.boundGateway) return;
    IrrigationProto::RemoteLed rl;
    if (!decodeRemoteLed(mp.decoded.payload.bytes, mp.decoded.payload.size, rl)) return;
    for (uint8_t s = 0; s < 2; s++)
        _remoteLed[s].onLedState((rl.ledStates >> s) & 1u, millis());
}
```

- [ ] **Step 5: Declarações** — `handleRemoteLed` em `IrrigationModule.h`.

- [ ] **Step 6: Build nativo**

Run: `pio run -e native`
Expected: SUCCESS.

- [ ] **Step 7: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): cola estação do modo remoto (borda->trigger, REMOTE_LED->FSM->pino)"
```

---

## Task 8: Endpoints CI-only — /api/irrigation/remote[/delete|/command]

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (accessors `remoteBuild*`/`remoteApply*` públicos, como os `mirror*`)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp`

> **CI/banca-only** — este arquivo é excluído do build nativo (`variants/native/portduino.ini`). Verificação = **build ESP32 no CI**.

- [ ] **Step 1: Accessors no módulo** — espelhando os `mirror*` (`IrrigationModule.h:139` + `IrrigationModule.cpp:3055`): `size_t remoteBuildStatus(char*, size_t)` (chama `IrrigationWeb::buildRemoteStatus` com a tabela+zonas+estado de abertura), `bool remoteApplyUpsert(const char *json, size_t)` (parse→`gwApplyRemoteUpsert`), `bool remoteApplyDelete(const char*, size_t)`, `bool remoteRunCommand(const char*, size_t)` (parse→`gwRunRemoteCommand`).

- [ ] **Step 2: Handlers** — em `IrrigationWebEndpoints.cpp`, espelhar `hMirror`/`hMirrorMapping`/`hMirrorMappingDelete` (`:790-889`), gated `role==GATEWAY`:

```cpp
static void hRemote(HTTPRequest *req, HTTPResponse *res) { /* GET → remoteBuildStatus */ }
static void hRemoteUpsert(HTTPRequest *req, HTTPResponse *res) { /* POST body → remoteApplyUpsert */ }
static void hRemoteDelete(HTTPRequest *req, HTTPResponse *res) { /* POST → remoteApplyDelete */ }
static void hRemoteCommand(HTTPRequest *req, HTTPResponse *res) { /* POST → remoteRunCommand */ }
```

  Ler corpo/escrever resposta idêntico aos `hMirror*` (mesmos helpers de leitura de body e `res->print`).

- [ ] **Step 3: Registro** — junto aos `registerNode` do mirror (`:1184-1187`):

```cpp
    server->registerNode(new ResourceNode("/api/irrigation/remote", "GET", &hRemote));
    server->registerNode(new ResourceNode("/api/irrigation/remote", "POST", &hRemoteUpsert));
    server->registerNode(new ResourceNode("/api/irrigation/remote/delete", "POST", &hRemoteDelete));
    server->registerNode(new ResourceNode("/api/irrigation/remote/command", "POST", &hRemoteCommand));
```

- [ ] **Step 4: Verificação** — o nativo **não** compila este arquivo; confirmar que segue excluído (nenhuma mudança em `portduino.ini`). O build ESP32 do CI valida. Rodar a suíte web nativa para garantir que os accessors do módulo linkam onde usados:

Run: `pio test -e native --filter test_irrigation_webapi`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationWebEndpoints.cpp
git commit -m "feat(irrigation): endpoints CI-only /api/irrigation/remote[/delete|/command]"
```

---

## Task 9: Frontend — painel "Mais → Modo Remoto" + mock

**Files:**
- Modify: `data/irrigacao/index.html`
- Modify: `data/irrigacao/app.js`
- Modify: `data/irrigacao/mock.js`

> Baseado no **mockup** `C:\Users\Jefferson\Downloads\Irrigacao Mobile.dc.html` (aba Mais → Modo Remoto, L1153–1313 / view-model L2097–2217, L3355–3451), com os refinamentos: **sem pill "Ativo" global**, **dedup por saída** no card de teste, **LED por gatilho** (nó da botoeira). Verificação = `node --check` + preview offline via `mock.js` + banca.

- [ ] **Step 1: Entrada no menu "Mais"** — em `app.js`, na lista de `renderMais` (`:3067-3069`, onde está `['espelhamento', 'Modo Espelhamento', …]`), acrescentar `['remoto', 'Modo Remoto', 'Botoeira em um nó/gateway aciona saída em outro']`. Registrar `remoto: renderRemoto` no mapa de renderers (`:3716`), rótulo em `:3730`, e `remoto: 1` em `POLLED` (`:3735`) para atualizar estado ao vivo.

- [ ] **Step 2: `renderRemoto` (lista)** — espelhar `renderEspelhamento` (`:2573`). Buscar `GET /api/irrigation/remote`. Renderizar:
  - **Card "Testar acionamento"** — iterar sobre `status[]` (já deduplicado por saída pelo backend): cada linha = rótulo da saída + estado ao vivo (ponto verde/cinza + "ligada/desligada") + botão **Acionar** → `POST /api/irrigation/remote/command {targetZoneId}`.
  - **Lista de associações** — cada card: rótulo da saída, "Acionado por: <nós>", estado, toggle Habilitada/Desativada (`POST /api/irrigation/remote` com `enabled` alternado), clique → editor. **Sem pill de modo global.**
  - Botão **+ Nova associação** → editor vazio.

- [ ] **Step 3: `renderRemoto` (editor)** — espelhar a tela de edição do espelhamento (`:2336+`) + o view-model do mockup (L3399–3451):
  - Seleção de **nós+entradas de gatilho** (chips): escolhe nó (Gateway/estações via `/overview` `selfNode` + `/stations`), depois a entrada digital (0..3). **Um input por nó** (selecionar outro no mesmo nó substitui — igual `toggleRemoteDraftTrigger` do mockup).
  - **Saída-alvo** = zona (lista de `/zones`; incluir "Gateway (local)" como nos outros forms de zona).
  - **LED por gatilho**: para cada gatilho selecionado, escolher **LED 1 / LED 2 / nenhum** (grava `ledSlot` 0/1/255).
  - Toggle habilitar. Botão **Salvar** → `POST /api/irrigation/remote` com `{id, enabled, targetZoneId, triggers:[{node,inputIdx,ledSlot}]}`. Excluir → `POST /api/irrigation/remote/delete {id}`.

- [ ] **Step 4: `index.html`** — se a navegação "Mais" for data-driven pelo `app.js`, nenhuma mudança estrutural; caso haja marcação estática para sub-telas, adicionar o container da sub-tela `remoto` espelhando o de `espelhamento`.

- [ ] **Step 5: `mock.js`** — adicionar rotas `GET/POST /api/irrigation/remote`, `/remote/delete`, `/remote/command` com um estado em memória: 2 associações de exemplo, **ambas apontando à mesma saída** (para exercitar o dedup do card), com `ledSlot` distintos; `command` alterna o estado da saída no mock.

- [ ] **Step 6: Verificação**

Run: `node --check data/irrigacao/app.js && node --check data/irrigacao/mock.js`
Expected: sem erros. (Opcional: abrir `data/irrigacao/index.html` com `mock.js` descomentado e conferir a aba.)

- [ ] **Step 7: Commit**

```bash
git add data/irrigacao/index.html data/irrigacao/app.js data/irrigacao/mock.js
git commit -m "feat(irrigation): painel Mais->Modo Remoto (dedup de saída, LED por gatilho) + mock"
```

---

## Task 10: Backup/restore da RemoteButtonTable (§5.5)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (`gwBuildBackup`, `gwImportTables`/`importConfigTablesFromBackup`)
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp` (se o import puro viver lá) ou `ServiceBackup`
- Test: `test/test_service_backup/test_main.cpp` (ou `test/test_irrigation_webapi/test_main.cpp` conforme onde o parser de import reside)

**Interfaces:**
- Consumes: `RemoteButtonTable`, `buildRemote`, scanner de import existente (`importConfigTablesFromBackup`).

- [ ] **Step 1: Teste que falha** — acrescentar caso ao suíte de backup: um envelope com `clients[0].config.remoteButtons:[…]` é parseado e a tabela reconstruída (round-trip do subconjunto), e `extractLight` tolera a seção nova (chave desconhecida ignorada por key-seek):

```cpp
static void test_import_remoteButtons()
{
    const char *env = "{\"clients\":[{\"config\":{\"remoteButtons\":["
        "{\"id\":1,\"enabled\":1,\"targetZoneId\":5,"
        "\"triggers\":[{\"node\":170,\"inputIdx\":2,\"ledSlot\":0}]}]}}]}";
    RemoteButtonTable t;
    size_t n = importRemoteButtonsFromBackup(env, strlen(env), t); // função nova pura
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_NOT_NULL(t.byId(1));
    TEST_ASSERT_EQUAL_UINT8(5, t.byId(1)->targetZoneId);
}
```

- [ ] **Step 2: Rodar e confirmar falha**

Run: `pio test -e native --filter test_service_backup`
Expected: FALHA — `importRemoteButtonsFromBackup` indefinida.

- [ ] **Step 3: Implementar** — `importRemoteButtonsFromBackup(const char*, size_t, RemoteButtonTable&)` pura, reusando o scanner de array aninhado de `importConfigTablesFromBackup` (mesmo `sliceToBuf`+parser por item). Em `gwBuildBackup`, emitir a seção `remoteButtons` (via `IrrigationWeb::buildRemote`) junto de zonas/grupos. Em `gwImportTables`, chamar `importRemoteButtonsFromBackup` e persistir (`gateway.remoteButtons` + `saveRemoteButtons`). Garantir que `extractLight` (chaves desconhecidas) já ignora a seção nova — adicionar caso de tolerância se necessário.

- [ ] **Step 4: Rodar e confirmar sucesso**

Run: `pio test -e native --filter test_service_backup`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.cpp src/modules/irrigation/IrrigationWebApi.cpp test/test_service_backup/test_main.cpp
git commit -m "feat(irrigation): backup/restore da RemoteButtonTable (§5.5)"
```

---

## Task 11: Verificação final — suíte completa + roadmap

**Files:**
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (registrar a fase Modo Remoto)

- [ ] **Step 1: Suíte nativa COMPLETA** (Docker, ver MEMORY.md):

Run: `./bin/run-tests.sh` (ou o fluxo Docker do MEMORY.md)
Expected: exit 0 GREEN; `71` suítes; baseline + os casos novos (protocolo ×2, config ×2, webapi ×2, remote_button_table ×4, remote_button ×3, service_backup ×1).

- [ ] **Step 2: `native --check` do front**

Run: `node --check data/irrigacao/app.js && node --check data/irrigacao/mock.js`
Expected: OK.

- [ ] **Step 3: Roadmap** — acrescentar entrada da fase "Modo Remoto" com resumo (aditivo v7, 2 msgs, gateway-central, banca+ESP32 exigidos).

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "docs(irrigation): roadmap — fase Modo Remoto concluída"
```

- [ ] **Step 5: Push ao fork** (multi-computador, ver MEMORY.md → firmware-git-remotes):

```bash
git push fork sistema-irrigacao
```

---

## Riscos / notas de banca (não native-testáveis)

- **Banca 2+ nós OBRIGATÓRIA**: `REMOTE_TRIGGER`/`REMOTE_LED` por rádio, toggle real, LED pisca→fixo→apaga, estado compartilhado (scheduler liga → LED acende sem pressão), falha (nó alvo fora → LED expira).
- **Build ESP32 no CI OBRIGATÓRIO**: endpoints webserver-guarded + pinos de LED no variant não compilam no nativo.
- **Re-push de config de estação**: salvar uma associação com gatilho numa estação dispara `SET_CONFIG` (epoch+1) — confirmar na banca que a estação passa a gerar trigger e a piscar após adotar.
- **Papel exclusivo de entrada**: a UI deve impedir marcar como botão uma entrada já usada por mirror/sensor (validação no editor; reforço no `validateRemoteTriggers` do lado do gateway é opcional — follow-up se necessário).
