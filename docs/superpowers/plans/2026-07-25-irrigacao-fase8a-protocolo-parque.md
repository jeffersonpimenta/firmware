# Irrigação Fase 8a — Pré-requisitos de protocolo no parque (Implementation Plan)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dar ao parque instalado (estações + gateways) os três comportamentos de protocolo que §11.9 exige antes de o device SERVICO existir: aceitar comando marcado `SERVICE_MAGIC`, responder `RESYNC_SEQ`, e responder à sonda `PING_SURVEY`.

**Architecture:** Aditivo sobre o codec existente (`IrrigationProtocol`) e o dispatch de `IrrigationModule`. Lógica pura (codec + predicado de autorização) é native-testada (Unity, como `SeqTable`/`RateLimiter`); o wiring dos handlers é ESP32-glue, validado por compilação nativa (o `IrrigationModule.cpp` compila no build native) + banca de hardware. Nenhum arquivo novo, nenhum módulo novo.

**Tech Stack:** C++11, Unity (test framework), PlatformIO env `coverage` (native) rodado em Docker (host Windows, ver memória `windows-native-test-docker`). Codec little-endian explícito via helpers `Writer`/`Reader` no namespace anônimo de `IrrigationProtocol.cpp`.

## Global Constraints

Copiar verbatim; valem em toda task:

- **Protocolo `IrrigationProto::VERSION` permanece `1`** — mudanças são aditivas; nós antigos ignoram flag/tipos novos.
- **ABI de settings intacta** — `IrrigationSettings` continua v5, 176 B; nenhuma task muda `IrrigationSettings.*`.
- **`SeqTable` fica RAM-only** — nenhuma persistência nova (decisão de brainstorming; posse-da-PSK=autoridade).
- **Payload ≤ 200 B** (`MAX_PAYLOAD`); header 8 B (`HEADER_LEN`); `flags` no offset 2 (little-endian).
- **`FLAG_FROM_SERVICE` não autentica** — é marcador; a autorização real é a posse da PSK do canal (spec §11.1).
- **Suíte nativa completa deve terminar 56/56 GREEN** — sem suite nova; casos vão para `test_irrigation_protocol` e `test_irrigation_replay`.
- **Teste nativo só roda em Docker** neste host — ver memória `windows-native-test-docker` (nunca `bin/test-native-docker.sh` direto; um container por vez; copiar src+test+variants+platformio.ini para o cache).

**Comando Docker por suite** (substituir `<suite>` e `<name>`):
```
MSYS_NO_PATHCONV=1 docker run --rm --name <name> -u 0 -e HOME=/root \
  -v "$(pwd -W 2>/dev/null || pwd):/src:ro" -v pio-cache:/root -v fwtest-cache:/tmp/fw-test \
  meshtastic-native-test timeout -k 30 1800 bash -c \
  "cp -a /src/src/. /tmp/fw-test/src/ && cp -a /src/test/. /tmp/fw-test/test/ && \
   cp -a /src/variants/. /tmp/fw-test/variants/ && cp -f /src/platformio.ini /tmp/fw-test/ && \
   cd /tmp/fw-test && platformio test -e coverage -f <suite> -vv"
```

---

## Task 1: `FLAG_FROM_SERVICE` + `setServiceFlag` (marcação de serviço no header)

Marca um pacote já codificado como originado por nó de serviço, setando o bit no campo `flags` (offset 2). Leitura é via `decodeHeader` (já lê `flags`). Base do SERVICE_MAGIC.

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h` (adicionar constante + declaração)
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp` (implementar `setServiceFlag`)
- Test: `test/test_irrigation_protocol/test_main.cpp`

**Interfaces:**
- Produces: `constexpr uint16_t IrrigationProto::FLAG_FROM_SERVICE = 0x0001;` · `void IrrigationProto::setServiceFlag(uint8_t *buf, size_t len);`

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_protocol/test_main.cpp`, adicionar (após `test_cmdValvula_roundTrip`):
```cpp
static void test_serviceFlag_setAndRead()
{
    uint8_t buf[MAX_PAYLOAD];
    CmdValvula in = {0, 1, 600};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 5, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h0;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h0));
    TEST_ASSERT_EQUAL_UINT16(0, h0.flags & FLAG_FROM_SERVICE); // default: sem marca

    setServiceFlag(buf, n);

    Header h1;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h1));
    TEST_ASSERT_TRUE((h1.flags & FLAG_FROM_SERVICE) != 0); // marcado
    // corpo intacto após marcar
    CmdValvula out;
    TEST_ASSERT_TRUE(decodeCmdValvula(buf, n, out));
    TEST_ASSERT_EQUAL_UINT16(600, out.durationS);
}
```
E registrar em `setup()` (junto dos outros `RUN_TEST`):
```cpp
    RUN_TEST(test_serviceFlag_setAndRead);
```

- [ ] **Step 2: Rodar e ver falhar (compile-fail: símbolos ausentes)**

Run: comando Docker com `<suite>=test_irrigation_protocol`, `<name>=fw8a-proto`.
Expected: FALHA de compilação — `FLAG_FROM_SERVICE`/`setServiceFlag` não declarados.

- [ ] **Step 3: Declarar no header**

Em `src/modules/irrigation/IrrigationProtocol.h`, logo após `constexpr uint8_t VERSION = 1;`:
```cpp
constexpr uint16_t FLAG_FROM_SERVICE = 0x0001; // Header.flags: remetente é nó de serviço (§11.5)
```
E na lista de assinaturas (junto de `uint32_t crc32(...)`):
```cpp
// Marca um pacote já codificado como originado pelo nó de serviço (§11.5); flags no offset 2 (LE).
void setServiceFlag(uint8_t *buf, size_t len);
```

- [ ] **Step 4: Implementar no .cpp**

Em `src/modules/irrigation/IrrigationProtocol.cpp`, antes de `uint32_t crc32(...)`:
```cpp
void setServiceFlag(uint8_t *buf, size_t len)
{
    if (len >= HEADER_LEN)
        buf[2] |= (uint8_t)(FLAG_FROM_SERVICE & 0xff); // flags LE começa no offset 2
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: comando Docker `<suite>=test_irrigation_protocol`.
Expected: PASS (todos os casos, incluindo `test_serviceFlag_setAndRead`).

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationProtocol.cpp test/test_irrigation_protocol/test_main.cpp
git commit -m "feat(irrigation): 8a — FLAG_FROM_SERVICE + setServiceFlag (marca de serviço no header)"
```

---

## Task 2: Predicado puro `senderAuthorizedBy`

Extrai a decisão de autorização para função livre testável. O membro do módulo (Task 5) passa a delegar.

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h` (função inline)
- Test: `test/test_irrigation_replay/test_main.cpp`

**Interfaces:**
- Consumes: `FLAG_FROM_SERVICE` (Task 1)
- Produces: `inline bool IrrigationProto::senderAuthorizedBy(uint16_t flags, uint32_t from, uint32_t boundGateway);`

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_replay/test_main.cpp`, no topo garantir includes:
```cpp
#include "modules/irrigation/IrrigationProtocol.h"
```
E `using namespace IrrigationProto;` logo após os includes (antes de `setUp`). Adicionar casos:
```cpp
static void test_senderAuthorized_unbound_acceptsAny()
{
    TEST_ASSERT_TRUE(senderAuthorizedBy(0, 0x1234, 0)); // boundGateway==0 → modo aberto
}
static void test_senderAuthorized_bound_onlyGateway()
{
    TEST_ASSERT_TRUE(senderAuthorizedBy(0, 0xAAAA, 0xAAAA));  // from==gateway vinculado
    TEST_ASSERT_FALSE(senderAuthorizedBy(0, 0xBBBB, 0xAAAA)); // outro nó → rejeitado
}
static void test_senderAuthorized_serviceFlag_bypassesBinding()
{
    TEST_ASSERT_TRUE(senderAuthorizedBy(FLAG_FROM_SERVICE, 0xBBBB, 0xAAAA)); // marca dispensa vínculo
}
```
Registrar em `setup()`:
```cpp
    RUN_TEST(test_senderAuthorized_unbound_acceptsAny);
    RUN_TEST(test_senderAuthorized_bound_onlyGateway);
    RUN_TEST(test_senderAuthorized_serviceFlag_bypassesBinding);
```

- [ ] **Step 2: Rodar e ver falhar**

Run: comando Docker `<suite>=test_irrigation_replay`, `<name>=fw8a-replay`.
Expected: FALHA de compilação — `senderAuthorizedBy` não declarado.

- [ ] **Step 3: Implementar a função inline**

Em `src/modules/irrigation/IrrigationProtocol.h`, após `FLAG_FROM_SERVICE`:
```cpp
// true = remetente autorizado a comandar (§4.2, §11.5). Puro: sem estado.
inline bool senderAuthorizedBy(uint16_t flags, uint32_t from, uint32_t boundGateway)
{
    if (flags & FLAG_FROM_SERVICE)
        return true;                                  // marca de serviço dispensa vínculo (§11.5)
    return boundGateway == 0 || from == boundGateway; // posse-da-PSK / vínculo (§4.2)
}
```

- [ ] **Step 4: Rodar e ver passar**

Run: comando Docker `<suite>=test_irrigation_replay`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.h test/test_irrigation_replay/test_main.cpp
git commit -m "feat(irrigation): 8a — predicado puro senderAuthorizedBy (SERVICE_MAGIC + vínculo)"
```

---

## Task 3: Codec `RESYNC_SEQ`

Corpo do `MSG_RESYNC_SEQ` (tipo 11, já no enum): `kind` (0=REQUEST/1=REPLY) + `lastSeq` (válido no REPLY).

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h` (struct + assinaturas)
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp` (encode/decode)
- Test: `test/test_irrigation_protocol/test_main.cpp`

**Interfaces:**
- Produces: `struct IrrigationProto::ResyncSeq { uint8_t kind; uint32_t lastSeq; };` · `size_t encodeResyncSeq(uint8_t*, size_t, uint32_t seq, const ResyncSeq&);` · `bool decodeResyncSeq(const uint8_t*, size_t, ResyncSeq&);`

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_protocol/test_main.cpp`:
```cpp
static void test_resyncSeq_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    ResyncSeq in = {1, 4242}; // REPLY carregando lastSeq
    size_t n = encodeResyncSeq(buf, sizeof(buf), 7, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_RESYNC_SEQ, h.type);

    ResyncSeq out;
    TEST_ASSERT_TRUE(decodeResyncSeq(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(1, out.kind);
    TEST_ASSERT_EQUAL_UINT32(4242, out.lastSeq);
}
```
Registrar em `setup()`: `RUN_TEST(test_resyncSeq_roundTrip);`

- [ ] **Step 2: Rodar e ver falhar**

Run: Docker `<suite>=test_irrigation_protocol`.
Expected: FALHA de compilação — `ResyncSeq`/`encodeResyncSeq`/`decodeResyncSeq` ausentes.

- [ ] **Step 3: Declarar no header**

Em `IrrigationProtocol.h`, junto das outras structs de mensagem:
```cpp
struct ResyncSeq {
    uint8_t kind;     // 0 = REQUEST (CTRL→EST), 1 = REPLY (EST→CTRL)
    uint32_t lastSeq; // válido só no REPLY: last_seq registrado p/ o remetente do REQUEST
};
```
E nas assinaturas:
```cpp
size_t encodeResyncSeq(uint8_t *buf, size_t len, uint32_t seq, const ResyncSeq &m);
bool decodeResyncSeq(const uint8_t *buf, size_t len, ResyncSeq &out);
```

- [ ] **Step 4: Implementar no .cpp**

Em `IrrigationProtocol.cpp` (antes de `} // namespace IrrigationProto`):
```cpp
size_t encodeResyncSeq(uint8_t *buf, size_t len, uint32_t seq, const ResyncSeq &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_RESYNC_SEQ, seq);
    w.u8(m.kind);
    w.u32(m.lastSeq);
    return w.ok ? w.pos : 0;
}

bool decodeResyncSeq(const uint8_t *buf, size_t len, ResyncSeq &out)
{
    Reader r = bodyReader(buf, len);
    out.kind = r.u8();
    out.lastSeq = r.u32();
    return r.ok;
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: Docker `<suite>=test_irrigation_protocol`. Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationProtocol.cpp test/test_irrigation_protocol/test_main.cpp
git commit -m "feat(irrigation): 8a — codec RESYNC_SEQ (kind + lastSeq)"
```

---

## Task 4: Codec `PING_SURVEY` + `APP_FW_VERSION`

Corpo do `MSG_PING_SURVEY` (tipo 10, já no enum): `kind` (0=PROBE/1=REPLY) + nodeinfo mínimo no REPLY.

**Files:**
- Modify: `src/modules/irrigation/IrrigationProtocol.h` (const + struct + assinaturas)
- Modify: `src/modules/irrigation/IrrigationProtocol.cpp` (encode/decode)
- Test: `test/test_irrigation_protocol/test_main.cpp`

**Interfaces:**
- Produces: `constexpr uint16_t IrrigationProto::APP_FW_VERSION = 0x0800;` · `struct IrrigationProto::PingSurvey { uint8_t kind; uint8_t role; uint32_t configEpoch; uint16_t vbatCentiV; uint16_t fwVersion; int32_t latE7; int32_t lonE7; };` · `size_t encodePingSurvey(uint8_t*, size_t, uint32_t seq, const PingSurvey&);` · `bool decodePingSurvey(const uint8_t*, size_t, PingSurvey&);`

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_protocol/test_main.cpp`:
```cpp
static void test_pingSurvey_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    PingSurvey in = {};
    in.kind = 1; // REPLY
    in.role = 1; // GATEWAY
    in.configEpoch = 17;
    in.vbatCentiV = 1250;
    in.fwVersion = APP_FW_VERSION;
    in.latE7 = -221000000;
    in.lonE7 = -476000000;
    size_t n = encodePingSurvey(buf, sizeof(buf), 9, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_PING_SURVEY, h.type);

    PingSurvey out;
    TEST_ASSERT_TRUE(decodePingSurvey(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(1, out.kind);
    TEST_ASSERT_EQUAL_UINT8(1, out.role);
    TEST_ASSERT_EQUAL_UINT32(17, out.configEpoch);
    TEST_ASSERT_EQUAL_UINT16(1250, out.vbatCentiV);
    TEST_ASSERT_EQUAL_UINT16(APP_FW_VERSION, out.fwVersion);
    TEST_ASSERT_EQUAL_INT32(-221000000, out.latE7);
    TEST_ASSERT_EQUAL_INT32(-476000000, out.lonE7);
}
```
Registrar em `setup()`: `RUN_TEST(test_pingSurvey_roundTrip);`

- [ ] **Step 2: Rodar e ver falhar**

Run: Docker `<suite>=test_irrigation_protocol`.
Expected: FALHA de compilação — símbolos `PingSurvey`/`APP_FW_VERSION`/encode/decode ausentes.

- [ ] **Step 3: Declarar no header**

Em `IrrigationProtocol.h`, após `constexpr uint16_t FLAG_FROM_SERVICE ...`:
```cpp
constexpr uint16_t APP_FW_VERSION = 0x0800; // geração do firmware de irrigação (Fase 8a); p/ planejamento de OTA (§3.3/§11.4)
```
Junto das structs de mensagem:
```cpp
struct PingSurvey {
    uint8_t kind;         // 0 = PROBE (sonda, req), 1 = REPLY
    uint8_t role;         // IrrigationRole (válido no REPLY)
    uint32_t configEpoch; // válido no REPLY
    uint16_t vbatCentiV;  // válido no REPLY
    uint16_t fwVersion;   // válido no REPLY (APP_FW_VERSION do respondente)
    int32_t latE7;        // válido no REPLY (0 se sem coordenada)
    int32_t lonE7;
};
```
E nas assinaturas:
```cpp
size_t encodePingSurvey(uint8_t *buf, size_t len, uint32_t seq, const PingSurvey &m);
bool decodePingSurvey(const uint8_t *buf, size_t len, PingSurvey &out);
```

- [ ] **Step 4: Implementar no .cpp**

Em `IrrigationProtocol.cpp` (antes de `} // namespace IrrigationProto`). `Writer`/`Reader` não têm `i32`; usa-se `u32` com cast (dois-complementos round-trip):
```cpp
size_t encodePingSurvey(uint8_t *buf, size_t len, uint32_t seq, const PingSurvey &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_PING_SURVEY, seq);
    w.u8(m.kind);
    w.u8(m.role);
    w.u32(m.configEpoch);
    w.u16(m.vbatCentiV);
    w.u16(m.fwVersion);
    w.u32((uint32_t)m.latE7);
    w.u32((uint32_t)m.lonE7);
    return w.ok ? w.pos : 0;
}

bool decodePingSurvey(const uint8_t *buf, size_t len, PingSurvey &out)
{
    Reader r = bodyReader(buf, len);
    out.kind = r.u8();
    out.role = r.u8();
    out.configEpoch = r.u32();
    out.vbatCentiV = r.u16();
    out.fwVersion = r.u16();
    out.latE7 = (int32_t)r.u32();
    out.lonE7 = (int32_t)r.u32();
    return r.ok;
}
```

- [ ] **Step 5: Rodar e ver passar**

Run: Docker `<suite>=test_irrigation_protocol`. Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationProtocol.h src/modules/irrigation/IrrigationProtocol.cpp test/test_irrigation_protocol/test_main.cpp
git commit -m "feat(irrigation): 8a — codec PING_SURVEY (nodeinfo mínimo) + APP_FW_VERSION"
```

---

## Task 5: Wiring SERVICE_MAGIC — `senderAuthorized(from, flags)` + 5 chamadores

Muda a assinatura do membro para receber `flags` e delegar ao predicado puro; atualiza os 5 chamadores para passar `h.flags`. Glue: sem teste nativo novo (predicado já coberto na Task 2); validação = compila + suíte GREEN.

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h:176` (assinatura)
- Modify: `src/modules/irrigation/IrrigationModule.cpp:283-288` (definição) e chamadores em `:376, :435, :481, :659, :730`

**Interfaces:**
- Consumes: `senderAuthorizedBy` (Task 2)
- Produces: `bool IrrigationModule::senderAuthorized(uint32_t from, uint16_t flags) const;` (assinatura nova usada pela Task 6 se preciso)

- [ ] **Step 1: Trocar a declaração no header**

`src/modules/irrigation/IrrigationModule.h:176`:
```cpp
    bool senderAuthorized(uint32_t from, uint16_t flags) const;
```

- [ ] **Step 2: Trocar a definição para delegar ao predicado puro**

`src/modules/irrigation/IrrigationModule.cpp:283-288` — substituir o corpo:
```cpp
bool IrrigationModule::senderAuthorized(uint32_t from, uint16_t flags) const
{
    // Predicado puro (§4.2, §11.5): marca de serviço OU não-vinculado OU vínculo com o remetente.
    return IrrigationProto::senderAuthorizedBy(flags, from, settings.boundGateway);
}
```

- [ ] **Step 3: Atualizar os 5 chamadores**

Cada um está dentro de um handler que tem `const Header &h` (ou `Header h`) em escopo. Trocar `senderAuthorized(mp.from)` por `senderAuthorized(mp.from, h.flags)` nas linhas `:376, :435, :481, :659, :730`. Padrão em cada sítio:
```cpp
    if (!senderAuthorized(mp.from, h.flags)) {
```

- [ ] **Step 4: Compilar + suíte completa GREEN**

Run: Docker sem `-f` (ou script `bin/run-tests.sh` via container) para a suíte completa; no mínimo rodar `<suite>=test_irrigation_protocol` e `<suite>=test_irrigation_replay` e um suite que force o link do módulo. Confirmar 56/56.
Expected: compila (assinatura nova sem chamador órfão) e todas GREEN.

- [ ] **Step 5: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): 8a — senderAuthorized(flags) aceita SERVICE_MAGIC nos 5 sítios de comando"
```

---

## Task 6: Responders `RESYNC_SEQ` + `PING_SURVEY` + dispatch

Adiciona os dois handlers e seus `case` no switch de `handleReceived`. Glue: validação = compila + suíte GREEN + banca de hardware (comportamento de rádio não é native-testável).

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarar 2 handlers, junto dos outros `handle*`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (dispatch em `:311` switch + implementações)

**Interfaces:**
- Consumes: `encodeResyncSeq`/`decodeResyncSeq` (Task 3), `encodePingSurvey`/`decodePingSurvey` + `APP_FW_VERSION` (Task 4); helpers existentes `allocDataPacket()`, `service->sendToMesh`, `packetPool.release`, `txSeq`, `rateLimiter`, `seqTable`, `batteryCentiV()`, `settings`.

- [ ] **Step 1: Declarar os handlers no header**

Em `src/modules/irrigation/IrrigationModule.h`, junto de `handleCmdValvula` etc.:
```cpp
    void handleResyncSeq(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handlePingSurvey(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
```

- [ ] **Step 2: Adicionar os `case` no switch de `handleReceived`**

Em `src/modules/irrigation/IrrigationModule.cpp`, no `switch (h.type)` (~linha 311), junto dos outros case:
```cpp
    case MSG_RESYNC_SEQ:
        handleResyncSeq(mp, h);
        break;
    case MSG_PING_SURVEY:
        handlePingSurvey(mp, h);
        break;
```

- [ ] **Step 3: Implementar `handleResyncSeq` (responder isento de anti-replay)**

Adicionar (perto de `sendAck`), seguindo o padrão de emissão de pacote do módulo:
```cpp
void IrrigationModule::handleResyncSeq(const meshtastic_MeshPacket &mp, const Header &h)
{
    ResyncSeq req;
    if (!decodeResyncSeq(mp.decoded.payload.bytes, mp.decoded.payload.size, req))
        return;
    if (req.kind != 0) // REPLY é consumido pelo controlador (gateway/8b); aqui só respondemos ao REQUEST
        return;
    // Query read-only: NÃO passa pelo gate anti-replay (§11.5 — o remetente pode ter contador defasado).
    if (!rateLimiter.allow(millis()))
        return;
    ResyncSeq reply = {};
    reply.kind = 1;
    reply.lastSeq = seqTable.lastSeq(mp.from);

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = mp.from;
    p->decoded.payload.size =
        (uint16_t)encodeResyncSeq(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, reply);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
}
```

- [ ] **Step 4: Implementar `handlePingSurvey` (responder em todos os papéis)**

```cpp
void IrrigationModule::handlePingSurvey(const meshtastic_MeshPacket &mp, const Header &h)
{
    PingSurvey req;
    if (!decodePingSurvey(mp.decoded.payload.bytes, mp.decoded.payload.size, req))
        return;
    if (req.kind != 0) // REPLY é coletado pelo prober (8b/8d); aqui só respondemos à PROBE
        return;
    // Sonda broadcast, resposta só nodeinfo: isenta de auth/seq, mas rate-limited p/ proteger airtime.
    if (!rateLimiter.allow(millis()))
        return;
    PingSurvey reply = {};
    reply.kind = 1;
    reply.role = settings.role;
    reply.configEpoch = settings.configEpoch;
    reply.vbatCentiV = batteryCentiV();
    reply.fwVersion = APP_FW_VERSION;
    reply.latE7 = settings.latE7;
    reply.lonE7 = settings.lonE7;

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = mp.from;
    p->decoded.payload.size =
        (uint16_t)encodePingSurvey(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, reply);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
}
```

- [ ] **Step 5: Compilar + suíte completa GREEN**

Run: suíte completa em Docker (mínimo `test_irrigation_protocol`, `test_irrigation_replay` + link do módulo). Confirmar 56/56 e ausência de warning de `case` não tratado.
Expected: compila e GREEN.

- [ ] **Step 6: Commit**

```bash
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): 8a — responders RESYNC_SEQ + PING_SURVEY (dispatch, todos papéis)"
```

---

## Fechamento

- [ ] **Suíte nativa completa 56/56 GREEN** (Docker) — confirmação final.
- [ ] **Atualizar roadmap** `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (linha Fase 8): marcar 8a concluída, apontar spec+plano.
- [ ] **Atualizar memória** `irrigacao-mesh-project.md` com o head do 8a e o estado (8b/8c/8d pendentes).
- [ ] **Push ao fork** (`fork/sistema-irrigacao`) — usuário trabalha de múltiplos computadores.
- [ ] **Nota de campo (não-nativo):** `SERVICE_MAGIC`/`RESYNC_SEQ`/`PING_SURVEY` só têm efeito real entre binários atualizados; exigem **banca de 2+ nós** antes do uso em campo (o comportamento de rádio não é coberto pela suíte nativa) — pré-requisito para 8b.

## Self-review (feito na escrita)

- **Cobertura da spec:** §11.9 item 1 (SERVICE_MAGIC) → Tasks 1,2,5; item 2 RESYNC → Tasks 3,6 (per-sender já pronto); item 4 PING_SURVEY responder → Tasks 4,6; item 3 maior-epoch já pronto (fora). §2 decisões (RAM-only, bit de flag, isenção anti-replay, nodeinfo) todas refletidas. Escopo OUT (emissor de scan, portal, survey no gateway) não vira task — é 8b/8c/8d.
- **Sem placeholders:** todo passo tem código/comando concreto.
- **Consistência de tipos:** `senderAuthorizedBy(flags, from, boundGateway)` idêntico entre Tasks 2 e 5; `ResyncSeq{kind,lastSeq}` e `PingSurvey{kind,role,configEpoch,vbatCentiV,fwVersion,latE7,lonE7}` idênticos entre Tasks 3/4 (codec) e Task 6 (uso); `setServiceFlag(buf,len)` idêntico Task 1 ↔ uso futuro (8b/tests).
