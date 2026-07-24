# Fase 7b — Grupos hidráulicos: painel web, validação, roteamento — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dar ao operador controle e observabilidade dos grupos hidráulicos (7a) via painel web do gateway, com validação de membros e roteamento correto de todos os caminhos de comando pelo motor.

**Architecture:** Segue o padrão 5a/6b — lógica pura native-testável em `IrrigationWebApi` (build/parse/validate), wiring ESP32 em `IrrigationWebEndpoints`, glue de estado no `IrrigationModule`, estáticos em `data/irrigacao/`. Nenhuma mudança de protocolo, settings ou blob de estação: grupos são orquestração 100% gateway (7a).

**Tech Stack:** C++17 (unidades puras + Unity native tests), esp32_https_server (endpoints, CI-only), LittleFS (persistência já existente), HTML/CSS/JS vanilla (painel).

## Global Constraints

- Todo endpoint e método de grupo é **GATEWAY-only** (`gwReady()` / `gwIsGateway()`), como as demais rotas do painel.
- **Sem mudança na ABI de estação** (protocolo, settings, blob de réplica v5). A bomba é uma zona gpo comum.
- **Teto fail-safe de 120 min** (`HydraulicGroupEngine::PUMP_CEILING_S = 7200`) vale para durações manuais.
- Unidades em `IrrigationWebApi` são **puras** — sem `Arduino.h`, sem estado de runtime; recebem tabelas/views por parâmetro.
- Validação semântica que precisa de `ZoneTable` fica em unidade pura (o header já inclui `GatewayTables.h`), **não** no módulo.
- Suite nativa deve permanecer **verde**; esta fase **não cria suite nova** (`test/native-suite-count` continua **56**) — casos entram em `test_irrigation_webapi` e `test_hydraulic_group_engine`.
- `trunk fmt` antes de cada commit.
- Persistência de grupos usa `saveGroups()` já existente (7a). Export/backup de grupos **fica fora** desta fase (adiado p/ Fase 8).
- Build de firmware ESP32 (tbeam) só é validável em **CI** — mudanças em módulo/endpoints/front são verificadas localmente por suite nativa verde + `trunk fmt` + revisão; CI confirma o build.

---

## File Structure

**Modify:**
- `src/modules/irrigation/HydraulicGroupEngine.h` / `.cpp` — 2 getters de observabilidade (`currentZone`, `openConfirmedCount`).
- `src/modules/irrigation/IrrigationWebApi.h` / `.cpp` — `GroupStatusView`, `buildGroups`, `buildGroupsStatus`, `groupStateLabel`, `parseGroupUpsert`, `parseGroupDelete`, `parseGroupCommand`, `validateGroupZones`.
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — `routeZoneToGroup` (helper) + rewire de 3 call-sites; `gwApplyGroupUpsert`, `gwApplyGroupDelete`, `gwRunGroupCommand`.
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — 5 handlers `hGroups*` + registro.
- `data/irrigacao/index.html` — botão da aba "Grupos".
- `data/irrigacao/app.js` — `renderGrupos`, `groupForm`, `pollGroupStatus`, entrada no `RENDER`, refresh.
- `data/irrigacao/style.css` — classe de cor de badge de estado (pequeno).
- `test/test_irrigation_webapi/test_main.cpp` — testes de build/parse/validate de grupos + `RUN_TEST`.
- `test/test_hydraulic_group_engine/test_main.cpp` — testes dos getters novos + `RUN_TEST`.
- `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` — marca 7b concluída.

**Create:** nenhum arquivo novo.

---

## Task 1: Getters de observabilidade no motor

**Files:**
- Modify: `src/modules/irrigation/HydraulicGroupEngine.h` (bloco público "Observabilidade p/ testes", ~linha 50-52)
- Modify: `src/modules/irrigation/HydraulicGroupEngine.cpp`
- Test: `test/test_hydraulic_group_engine/test_main.cpp`

**Interfaces:**
- Produces: `uint8_t HydraulicGroupEngine::currentZone(uint8_t groupId) const;` (zona corrente da máquina, 0 = nenhuma) e `uint8_t HydraulicGroupEngine::openConfirmedCount(uint8_t groupId) const;` (nº de zonas-membro confirmadas abertas). `groupId` 1..8; fora do range ⇒ 0.

- [ ] **Step 1: Escreve o teste que falha**

Em `test/test_hydraulic_group_engine/test_main.cpp`, adiciona (perto dos demais testes de estado; use o mesmo helper de setup de grupo do arquivo — uma `HydraulicGroupTable tbl` + `ZoneTable zones` já usados nos testes existentes):

```cpp
static void test_getters_currentZone_openCount()
{
    // Reusa o padrão de fixture do arquivo: 1 grupo, 2 zonas, minOpen=1, sem bomba.
    HydraulicGroupTable tbl;
    ZoneTable zones;
    seedGroupWithZones(tbl, zones, /*groupId*/ 1, /*z1*/ 3, /*z2*/ 4); // helper já existente no arquivo
    HydraulicGroupEngine eng;

    // Fora do range e ocioso: zero.
    TEST_ASSERT_EQUAL_UINT8(0, eng.currentZone(1));
    TEST_ASSERT_EQUAL_UINT8(0, eng.openConfirmedCount(1));
    TEST_ASSERT_EQUAL_UINT8(0, eng.currentZone(0));   // groupId inválido
    TEST_ASSERT_EQUAL_UINT8(0, eng.currentZone(9));   // groupId inválido

    // Deseja abrir z3; drena tick e confirma via observeActual.
    GroupEmit emits[8];
    eng.setDesired(1, 3, true, 60);
    eng.tick(tbl, zones, 1000, emits, 8);
    eng.observeActual(1, 3, true);
    eng.tick(tbl, zones, 2000, emits, 8);

    TEST_ASSERT_EQUAL_UINT8(1, eng.openConfirmedCount(1));
}
```

> Nota p/ o implementador: se `seedGroupWithZones` não existir com esse nome no arquivo, use a construção de `HydraulicGroup`/`Zone` inline como nos testes vizinhos (procure por `HydraulicGroup g{}` no arquivo) — repita o setup, não invente helper.

- [ ] **Step 2: Roda e confirma que falha**

Run: `./bin/run-tests.sh -f test_hydraulic_group_engine`
Expected: FAIL (compilação: `currentZone`/`openConfirmedCount` não declarados).

- [ ] **Step 3: Declara os getters no header**

Em `HydraulicGroupEngine.h`, no bloco público após `bool pumpOn(uint8_t groupId) const;`:

```cpp
    uint8_t currentZone(uint8_t groupId) const;        // rt.curZone; 0 = nenhuma / groupId inválido
    uint8_t openConfirmedCount(uint8_t groupId) const; // zonas-membro confirmadas abertas
```

- [ ] **Step 4: Implementa no .cpp**

Em `HydraulicGroupEngine.cpp` (perto de `stateOf`/`pumpOn`):

```cpp
uint8_t HydraulicGroupEngine::currentZone(uint8_t groupId) const
{
    if (groupId < 1 || groupId > MAX_GROUPS)
        return 0;
    return rt[groupId - 1].curZone;
}

uint8_t HydraulicGroupEngine::openConfirmedCount(uint8_t groupId) const
{
    if (groupId < 1 || groupId > MAX_GROUPS)
        return 0;
    const GroupRt &g = rt[groupId - 1];
    uint8_t n = 0;
    for (size_t i = 0; i < MAX_ZONES; i++)
        if (g.zones[i].zoneId != 0 && g.zones[i].confirmed)
            n++;
    return n;
}
```

- [ ] **Step 5: Registra o teste**

Adiciona `RUN_TEST(test_getters_currentZone_openCount);` na lista `main()` do arquivo.

- [ ] **Step 6: Roda e confirma verde**

Run: `./bin/run-tests.sh -f test_hydraulic_group_engine`
Expected: PASS (todos, incluindo o novo).

- [ ] **Step 7: fmt + commit**

```bash
trunk fmt src/modules/irrigation/HydraulicGroupEngine.h src/modules/irrigation/HydraulicGroupEngine.cpp test/test_hydraulic_group_engine/test_main.cpp
git add src/modules/irrigation/HydraulicGroupEngine.h src/modules/irrigation/HydraulicGroupEngine.cpp test/test_hydraulic_group_engine/test_main.cpp
git commit -m "feat(irrigation): grupo — getters currentZone/openConfirmedCount (fase 7b)"
```

---

## Task 2: Parsers puros (`parseGroupUpsert` / `parseGroupDelete` / `parseGroupCommand`)

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h` (declarações + include)
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp` (implementações)
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `HydraulicGroup` (de `HydraulicGroupTable.h`), `JsonReader`, `ParseResult`.
- Produces:
  - `ParseResult parseGroupUpsert(const char *json, size_t len, HydraulicGroup &out);` — validação **estrutural**. Aceita `id` 0..8 (0 = criar, servidor aloca).
  - `ParseResult parseGroupDelete(const char *json, size_t len, uint8_t &outId);`
  - `ParseResult parseGroupCommand(const char *json, size_t len, uint8_t &outId, bool &outOpen, uint16_t &outDurationS);`

- [ ] **Step 1: Include do tipo no header**

Em `IrrigationWebApi.h`, junto aos includes de tabelas (após `#include "modules/irrigation/InterlockTable.h"`):

```cpp
#include "modules/irrigation/HydraulicGroupTable.h"
```

- [ ] **Step 2: Declarações no header**

Em `IrrigationWebApi.h`, antes do `} // namespace IrrigationWeb`, numa seção nova:

```cpp
// ── Fase 7b — grupos hidráulicos ─────────────────────────────────────────────

ParseResult parseGroupUpsert(const char *json, size_t len, HydraulicGroup &out);
ParseResult parseGroupDelete(const char *json, size_t len, uint8_t &outId);
// {id, acao:"abrir"|"fechar", durationS?}. durationS omitido => 0 (módulo aplica default).
ParseResult parseGroupCommand(const char *json, size_t len, uint8_t &outId, bool &outOpen, uint16_t &outDurationS);
```

- [ ] **Step 3: Escreve os testes que falham**

Em `test/test_irrigation_webapi/test_main.cpp` (após os testes de interlock; adiciona `#include "modules/irrigation/HydraulicGroupTable.h"` no topo):

```cpp
static void test_parseGroupUpsert_ok()
{
    const char *j = "{\"id\":1,\"nome\":\"Norte\",\"bombaZoneId\":9,\"zonas\":[3,4,5],"
                    "\"minOpen\":2,\"maxOpen\":3,\"transicao\":1,\"overlapS\":10,"
                    "\"startAfterOpenS\":5,\"stopBeforeCloseS\":8,\"minRunMin\":5,\"maxStartsHour\":6}";
    HydraulicGroup out;
    ParseResult r = parseGroupUpsert(j, strlen(j), out);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(1, out.id);
    TEST_ASSERT_EQUAL_STRING("Norte", out.name);
    TEST_ASSERT_EQUAL_UINT8(9, out.bombaZoneId);
    TEST_ASSERT_EQUAL_UINT8(3, out.zoneCount);
    TEST_ASSERT_EQUAL_UINT8(3, out.zoneIds[0]);
    TEST_ASSERT_EQUAL_UINT8(5, out.zoneIds[2]);
    TEST_ASSERT_EQUAL_UINT8(2, out.minOpen);
    TEST_ASSERT_EQUAL_UINT8(3, out.maxOpen);
    TEST_ASSERT_EQUAL_UINT8(1, out.transicao);
}

static void test_parseGroupUpsert_idZeroAllowed()
{
    const char *j = "{\"id\":0,\"nome\":\"Nova\",\"zonas\":[2],\"minOpen\":1,\"maxOpen\":1}";
    HydraulicGroup out;
    ParseResult r = parseGroupUpsert(j, strlen(j), out);
    TEST_ASSERT_TRUE(r.ok);          // id=0 = criar (servidor aloca)
    TEST_ASSERT_EQUAL_UINT8(0, out.id);
    TEST_ASSERT_EQUAL_UINT8(1, out.zoneCount);
}

static void test_parseGroupUpsert_rejectsMinGtMax()
{
    const char *j = "{\"id\":1,\"zonas\":[3],\"minOpen\":3,\"maxOpen\":2}";
    HydraulicGroup out;
    TEST_ASSERT_FALSE(parseGroupUpsert(j, strlen(j), out).ok);
}

static void test_parseGroupUpsert_maxZeroMeansNoCeiling()
{
    const char *j = "{\"id\":1,\"zonas\":[3,4],\"minOpen\":2,\"maxOpen\":0}";
    HydraulicGroup out;
    TEST_ASSERT_TRUE(parseGroupUpsert(j, strlen(j), out).ok); // maxOpen=0 = sem teto, válido
}

static void test_parseGroupUpsert_rejectsNoZones()
{
    const char *j = "{\"id\":1,\"zonas\":[],\"minOpen\":1,\"maxOpen\":1}";
    HydraulicGroup out;
    TEST_ASSERT_FALSE(parseGroupUpsert(j, strlen(j), out).ok);
}

static void test_parseGroupUpsert_zonasOverflowGuard()
{
    const char *j = "{\"id\":1,\"zonas\":[1,2,3,4,5,6,7,8,9,10],\"minOpen\":1,\"maxOpen\":8}";
    HydraulicGroup out;
    ParseResult r = parseGroupUpsert(j, strlen(j), out);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(8, out.zoneCount); // trunca em 8, sem estourar
}

static void test_parseGroupDelete_ok()
{
    const char *j = "{\"id\":2}";
    uint8_t id = 0;
    TEST_ASSERT_TRUE(parseGroupDelete(j, strlen(j), id).ok);
    TEST_ASSERT_EQUAL_UINT8(2, id);
}

static void test_parseGroupDelete_rejectsBadId()
{
    const char *j0 = "{\"id\":0}";
    uint8_t id = 0;
    TEST_ASSERT_FALSE(parseGroupDelete(j0, strlen(j0), id).ok);
    const char *j9 = "{\"id\":9}";
    TEST_ASSERT_FALSE(parseGroupDelete(j9, strlen(j9), id).ok);
}

static void test_parseGroupCommand_abrir()
{
    const char *j = "{\"id\":1,\"acao\":\"abrir\",\"durationS\":300}";
    uint8_t id = 0; bool open = false; uint16_t dur = 0;
    ParseResult r = parseGroupCommand(j, strlen(j), id, open, dur);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(1, id);
    TEST_ASSERT_TRUE(open);
    TEST_ASSERT_EQUAL_UINT16(300, dur);
}

static void test_parseGroupCommand_fecharNoDur()
{
    const char *j = "{\"id\":2,\"acao\":\"fechar\"}";
    uint8_t id = 0; bool open = true; uint16_t dur = 99;
    ParseResult r = parseGroupCommand(j, strlen(j), id, open, dur);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(open);
    TEST_ASSERT_EQUAL_UINT16(0, dur); // omitido => 0
}

static void test_parseGroupCommand_rejectsBadAcao()
{
    const char *j = "{\"id\":1,\"acao\":\"xyz\"}";
    uint8_t id = 0; bool open = false; uint16_t dur = 0;
    TEST_ASSERT_FALSE(parseGroupCommand(j, strlen(j), id, open, dur).ok);
}
```

- [ ] **Step 4: Roda e confirma que falha**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FAIL (funções não definidas).

- [ ] **Step 5: Implementa os parsers**

Em `IrrigationWebApi.cpp`, seção nova (o parse do array `zonas` replica **exatamente** a técnica de `parseInterlockUpsert`, linhas ~447-482):

```cpp
// ── Fase 7b — grupos hidráulicos: parsers ────────────────────────────────────

ParseResult parseGroupUpsert(const char *json, size_t len, HydraulicGroup &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = -1, bomba = 0, minOpen = 1, maxOpen = 1, transicao = 0;
    int64_t overlap = 10, startAfter = 5, stopBefore = 8, minRun = 5, maxStarts = 6;
    char nome[16] = {0};

    if (!rd.getInt("id", id) || id < 0 || id > (int64_t)HydraulicGroupTable::MAX)
        r.fail("id invalido (0..8)");
    rd.getStr("nome", nome, sizeof(nome));
    rd.getInt("bombaZoneId", bomba);
    rd.getInt("minOpen", minOpen);
    rd.getInt("maxOpen", maxOpen);
    rd.getInt("transicao", transicao);
    rd.getInt("overlapS", overlap);
    rd.getInt("startAfterOpenS", startAfter);
    rd.getInt("stopBeforeCloseS", stopBefore);
    rd.getInt("minRunMin", minRun);
    rd.getInt("maxStartsHour", maxStarts);

    // array "zonas" — mesma técnica de parseInterlockUpsert
    const char *sp = strstr(json, "\"zonas\"");
    const char *arr = sp ? strchr(sp, '[') : nullptr;
    const char *arrEnd = arr ? strchr(arr, ']') : nullptr;
    uint8_t zoneIds[8] = {0};
    uint8_t zCount = 0;
    if (arr && arrEnd) {
        const char *o = arr + 1;
        while (o < arrEnd) {
            while (o < arrEnd && (*o < '0' || *o > '9')) o++;
            if (o >= arrEnd) break;
            char tok[8]; size_t ti = 0; const char *p2 = o; bool anyD = false;
            while (p2 < arrEnd && *p2 >= '0' && *p2 <= '9' && ti + 1 < sizeof(tok)) {
                tok[ti++] = *p2++; anyD = true;
            }
            if (anyD) {
                tok[ti] = '\0';
                char *tend = nullptr;
                int64_t zid = strtoll(tok, &tend, 10);
                if (tend != tok && zid > 0 && zCount < 8) zoneIds[zCount++] = (uint8_t)zid;
            }
            o = p2;
            while (o < arrEnd && *o != ',') o++;
            if (o < arrEnd) o++;
        }
    }

    if (zCount < 1) r.fail("grupo sem zonas");
    if (minOpen < 1) r.fail("minOpen >= 1");
    if (maxOpen != 0 && maxOpen < minOpen) r.fail("maxOpen < minOpen");
    if (transicao != 0 && transicao != 1) r.fail("transicao invalida");

    if (!r.ok) return r;

    out = HydraulicGroup{};
    out.id = (uint8_t)id;
    snprintf(out.name, sizeof(out.name), "%s", nome);
    out.bombaZoneId = (uint8_t)bomba;
    for (uint8_t z = 0; z < 8; z++) out.zoneIds[z] = (z < zCount) ? zoneIds[z] : 0;
    out.zoneCount = zCount;
    out.minOpen = (uint8_t)minOpen;
    out.maxOpen = (uint8_t)maxOpen;
    out.transicao = (uint8_t)transicao;
    out.overlapS = (uint16_t)overlap;
    out.startAfterOpenS = (uint16_t)startAfter;
    out.stopBeforeCloseS = (uint16_t)stopBefore;
    out.minRunMin = (uint16_t)minRun;
    out.maxStartsHour = (uint8_t)maxStarts;
    return r;
}

ParseResult parseGroupDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > (int64_t)HydraulicGroupTable::MAX) {
        r.fail("id invalido (1..8)");
        return r;
    }
    outId = (uint8_t)id;
    return r;
}

ParseResult parseGroupCommand(const char *json, size_t len, uint8_t &outId, bool &outOpen, uint16_t &outDurationS)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0, dur = 0;
    char acao[8] = {0};
    if (!rd.getInt("id", id) || id < 1 || id > (int64_t)HydraulicGroupTable::MAX)
        r.fail("id invalido (1..8)");
    if (!rd.getStr("acao", acao, sizeof(acao)))
        r.fail("acao ausente");
    rd.getInt("durationS", dur);

    bool open;
    if (strcmp(acao, "abrir") == 0)
        open = true;
    else if (strcmp(acao, "fechar") == 0)
        open = false;
    else {
        if (r.ok) r.fail("acao invalida (abrir|fechar)");
        return r;
    }
    if (!r.ok) return r;
    outId = (uint8_t)id;
    outOpen = open;
    outDurationS = (dur > 0 && dur <= 0xFFFF) ? (uint16_t)dur : 0;
    return r;
}
```

- [ ] **Step 6: Registra os testes**

Adiciona na lista `main()` de `test_irrigation_webapi/test_main.cpp`:

```cpp
    RUN_TEST(test_parseGroupUpsert_ok);
    RUN_TEST(test_parseGroupUpsert_idZeroAllowed);
    RUN_TEST(test_parseGroupUpsert_rejectsMinGtMax);
    RUN_TEST(test_parseGroupUpsert_maxZeroMeansNoCeiling);
    RUN_TEST(test_parseGroupUpsert_rejectsNoZones);
    RUN_TEST(test_parseGroupUpsert_zonasOverflowGuard);
    RUN_TEST(test_parseGroupDelete_ok);
    RUN_TEST(test_parseGroupDelete_rejectsBadId);
    RUN_TEST(test_parseGroupCommand_abrir);
    RUN_TEST(test_parseGroupCommand_fecharNoDur);
    RUN_TEST(test_parseGroupCommand_rejectsBadAcao);
```

- [ ] **Step 7: Roda e confirma verde**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: PASS.

- [ ] **Step 8: fmt + commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): grupo — parsers web (upsert/delete/command) (fase 7b)"
```

---

## Task 3: Validador semântico puro (`validateGroupZones`)

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: `HydraulicGroup`, `ZoneTable` (de `GatewayTables.h`, já incluído).
- Produces: `bool validateGroupZones(const HydraulicGroup &g, const ZoneTable &zones, char *err, size_t errCap);` — `true` = ok; `false` + `err` preenchido. Regras: toda zona-membro existe; **nenhuma zona-membro com `fonteInput >= 0`** (espelho); `bombaZoneId` (se ≠0) existe e não é membro.

- [ ] **Step 1: Declaração no header**

Em `IrrigationWebApi.h`, na seção Fase 7b:

```cpp
// Validação semântica (precisa da ZoneTable). true=ok; senão preenche err (>=48 bytes).
bool validateGroupZones(const HydraulicGroup &g, const ZoneTable &zones, char *err, size_t errCap);
```

- [ ] **Step 2: Escreve os testes que falham**

Em `test_irrigation_webapi/test_main.cpp`. Fixture de `ZoneTable`: use `Zone` (de `GatewayTables.h`) — campos `id`, `fonteInput` (−1 = não-espelho). Monte zonas com `zones.upsert(z)` (mesma API usada em `test_irrigation_gwtables`).

```cpp
static Zone mkZone(uint8_t id, int8_t fonte)
{
    Zone z{};
    z.id = id;
    z.node = 0x1111;
    z.fonteInput = fonte;
    return z;
}

static void test_validateGroupZones_ok()
{
    ZoneTable zones;
    zones.upsert(mkZone(3, -1));
    zones.upsert(mkZone(4, -1));
    zones.upsert(mkZone(9, -1)); // bomba
    HydraulicGroup g{};
    g.id = 1; g.zoneCount = 2; g.zoneIds[0] = 3; g.zoneIds[1] = 4; g.bombaZoneId = 9;
    char err[48] = {0};
    TEST_ASSERT_TRUE(validateGroupZones(g, zones, err, sizeof(err)));
}

static void test_validateGroupZones_rejectsMirrorMember()
{
    ZoneTable zones;
    zones.upsert(mkZone(3, -1));
    zones.upsert(mkZone(4, 0)); // zona 4 é espelho (fonteInput=0)
    HydraulicGroup g{};
    g.id = 1; g.zoneCount = 2; g.zoneIds[0] = 3; g.zoneIds[1] = 4;
    char err[48] = {0};
    TEST_ASSERT_FALSE(validateGroupZones(g, zones, err, sizeof(err)));
    TEST_ASSERT_TRUE(strlen(err) > 0);
}

static void test_validateGroupZones_rejectsMissingMember()
{
    ZoneTable zones;
    zones.upsert(mkZone(3, -1));
    HydraulicGroup g{};
    g.id = 1; g.zoneCount = 2; g.zoneIds[0] = 3; g.zoneIds[1] = 7; // 7 não existe
    char err[48] = {0};
    TEST_ASSERT_FALSE(validateGroupZones(g, zones, err, sizeof(err)));
}

static void test_validateGroupZones_rejectsPumpAsMember()
{
    ZoneTable zones;
    zones.upsert(mkZone(3, -1));
    zones.upsert(mkZone(9, -1));
    HydraulicGroup g{};
    g.id = 1; g.zoneCount = 2; g.zoneIds[0] = 3; g.zoneIds[1] = 9; g.bombaZoneId = 9; // bomba também membro
    char err[48] = {0};
    TEST_ASSERT_FALSE(validateGroupZones(g, zones, err, sizeof(err)));
}
```

- [ ] **Step 3: Roda e confirma que falha**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FAIL (função não definida).

- [ ] **Step 4: Implementa**

Em `IrrigationWebApi.cpp`:

```cpp
bool validateGroupZones(const HydraulicGroup &g, const ZoneTable &zones, char *err, size_t errCap)
{
    for (uint8_t i = 0; i < g.zoneCount && i < 8; i++) {
        uint8_t zid = g.zoneIds[i];
        const Zone *z = zones.byId(zid);
        if (!z) {
            snprintf(err, errCap, "zona %u inexistente", zid);
            return false;
        }
        if (z->fonteInput >= 0) {
            snprintf(err, errCap, "zona %u e espelho", zid);
            return false;
        }
        if (g.bombaZoneId != 0 && zid == g.bombaZoneId) {
            snprintf(err, errCap, "bomba %u nao pode ser membro", zid);
            return false;
        }
    }
    if (g.bombaZoneId != 0 && !zones.byId(g.bombaZoneId)) {
        snprintf(err, errCap, "bomba %u inexistente", g.bombaZoneId);
        return false;
    }
    return true;
}
```

- [ ] **Step 5: Registra os testes**

```cpp
    RUN_TEST(test_validateGroupZones_ok);
    RUN_TEST(test_validateGroupZones_rejectsMirrorMember);
    RUN_TEST(test_validateGroupZones_rejectsMissingMember);
    RUN_TEST(test_validateGroupZones_rejectsPumpAsMember);
```

- [ ] **Step 6: Roda e confirma verde**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: PASS.

- [ ] **Step 7: fmt + commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): grupo — validacao semantica de zonas (espelho/existencia) (fase 7b)"
```

---

## Task 4: Builders puros (`buildGroups`, `buildGroupsStatus`, `groupStateLabel`)

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.h`
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp`
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Produces:
  - `const char *groupStateLabel(uint8_t state);` — mapa `HydraulicGroupEngine::State` (uint8_t) → string estável.
  - `size_t buildGroups(const HydraulicGroupTable &tbl, char *buf, size_t cap);`
  - `struct GroupStatusView { uint8_t id; const char *name; uint8_t state; bool pump; uint8_t curZone; uint8_t openCount; };`
  - `size_t buildGroupsStatus(const GroupStatusView *views, size_t n, char *buf, size_t cap);`

- [ ] **Step 1: Declarações no header**

Em `IrrigationWebApi.h`, seção Fase 7b:

```cpp
const char *groupStateLabel(uint8_t state);
size_t buildGroups(const HydraulicGroupTable &tbl, char *buf, size_t cap);

struct GroupStatusView {
    uint8_t id = 0;
    const char *name = "";
    uint8_t state = 0;     // HydraulicGroupEngine::State
    bool pump = false;
    uint8_t curZone = 0;
    uint8_t openCount = 0;
};
size_t buildGroupsStatus(const GroupStatusView *views, size_t n, char *buf, size_t cap);
```

- [ ] **Step 2: Escreve os testes que falham**

```cpp
static void test_buildGroups_basic()
{
    HydraulicGroupTable tbl;
    HydraulicGroup g{};
    g.id = 1; snprintf(g.name, sizeof(g.name), "Norte");
    g.bombaZoneId = 9; g.zoneCount = 2; g.zoneIds[0] = 3; g.zoneIds[1] = 4;
    g.minOpen = 2; g.maxOpen = 3; g.transicao = 1;
    tbl.upsert(g);

    char buf[1024];
    size_t n = buildGroups(tbl, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"nome\":\"Norte\""));
    TEST_ASSERT_TRUE(contains(buf, "\"bombaZoneId\":9"));
    TEST_ASSERT_TRUE(contains(buf, "\"zonas\":[3,4]"));
    TEST_ASSERT_TRUE(contains(buf, "\"minOpen\":2"));
    TEST_ASSERT_TRUE(contains(buf, "\"maxOpen\":3"));
    TEST_ASSERT_TRUE(contains(buf, "\"transicao\":1"));
}

static void test_buildGroups_empty()
{
    HydraulicGroupTable tbl;
    char buf[64];
    size_t n = buildGroups(tbl, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("[]", buf);
}

static void test_groupStateLabel_map()
{
    TEST_ASSERT_EQUAL_STRING("ocioso", groupStateLabel(0));   // IDLE
    TEST_ASSERT_EQUAL_STRING("rodando", groupStateLabel(4));  // RUNNING
    TEST_ASSERT_EQUAL_STRING("transicao", groupStateLabel(6)); // X_OVERLAP
    TEST_ASSERT_EQUAL_STRING("adiado", groupStateLabel(11));  // DEFERRED
}

static void test_buildGroupsStatus_basic()
{
    GroupStatusView v{};
    v.id = 1; v.name = "Norte"; v.state = 4 /*RUNNING*/; v.pump = true; v.curZone = 4; v.openCount = 2;
    char buf[512];
    size_t n = buildGroupsStatus(&v, 1, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"estado\":\"rodando\""));
    TEST_ASSERT_TRUE(contains(buf, "\"bomba\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"zonaCorrente\":4"));
    TEST_ASSERT_TRUE(contains(buf, "\"abertas\":2"));
}
```

- [ ] **Step 3: Roda e confirma que falha**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: FAIL.

- [ ] **Step 4: Implementa**

Em `IrrigationWebApi.cpp` (o mapa segue a ordem do enum `HydraulicGroupEngine::State`):

```cpp
const char *groupStateLabel(uint8_t state)
{
    switch (state) {
    case 0: return "ocioso";              // IDLE
    case 1: return "abrindo";             // OPENING
    case 2: return "aguardando_partida";  // START_WAIT
    case 3: return "partindo_bomba";      // PUMP_WAIT_ACK
    case 4: return "rodando";             // RUNNING
    case 5:                               // X_OPEN_WAIT
    case 6:                               // X_OVERLAP
    case 7: return "transicao";           // X_CLOSE_WAIT
    case 8: return "parando_bomba";       // PUMP_OFF_WAIT
    case 9: return "drenando";            // DRAIN
    case 10: return "fechando";           // CLOSE_LAST_WAIT
    case 11: return "adiado";             // DEFERRED
    default: return "desconhecido";
    }
}

size_t buildGroups(const HydraulicGroupTable &tbl, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < tbl.count(); i++) {
        const HydraulicGroup *g = tbl.groupAt(i);
        if (!g) break;
        w.beginObject();
        w.keyNum("id", g->id);
        w.keyStr("nome", g->name);
        w.keyNum("bombaZoneId", g->bombaZoneId);
        w.key("zonas");
        w.beginArray();
        for (uint8_t z = 0; z < g->zoneCount && z < 8; z++) w.num(g->zoneIds[z]);
        w.endArray();
        w.keyNum("minOpen", g->minOpen);
        w.keyNum("maxOpen", g->maxOpen);
        w.keyNum("transicao", g->transicao);
        w.keyNum("overlapS", g->overlapS);
        w.keyNum("startAfterOpenS", g->startAfterOpenS);
        w.keyNum("stopBeforeCloseS", g->stopBeforeCloseS);
        w.keyNum("minRunMin", g->minRunMin);
        w.keyNum("maxStartsHour", g->maxStartsHour);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

size_t buildGroupsStatus(const GroupStatusView *views, size_t n, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const GroupStatusView &v = views[i];
        w.beginObject();
        w.keyNum("id", v.id);
        w.keyStr("nome", v.name ? v.name : "");
        w.keyStr("estado", groupStateLabel(v.state));
        w.keyBool("bomba", v.pump);
        w.keyNum("zonaCorrente", v.curZone);
        w.keyNum("abertas", v.openCount);
        w.endObject();
    }
    w.endArray();
    return w.done();
}
```

- [ ] **Step 5: Registra os testes**

```cpp
    RUN_TEST(test_buildGroups_basic);
    RUN_TEST(test_buildGroups_empty);
    RUN_TEST(test_groupStateLabel_map);
    RUN_TEST(test_buildGroupsStatus_basic);
```

- [ ] **Step 6: Roda e confirma verde**

Run: `./bin/run-tests.sh -f test_irrigation_webapi`
Expected: PASS.

- [ ] **Step 7: fmt + commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git add src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebApi.cpp test/test_irrigation_webapi/test_main.cpp
git commit -m "feat(irrigation): grupo — builders web (config + status ao vivo) (fase 7b)"
```

---

## Task 5: Roteamento único de comando de zona (`routeZoneToGroup`) + rewire dos 3 call-sites

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declaração privada)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (helper + rewire em `handleWebCommand` ~1755-1772, `portalRunNetCommand` ~1829-1842, scheduler `gwTick` ~1995-2013)

**Interfaces:**
- Produces: `bool IrrigationModule::routeZoneToGroup(uint8_t zoneId, bool open, uint16_t durationS);` — se a zona pertence a um grupo, aplica `groupEngine.setDesired` (respeitando intertravamento no OPEN) e retorna `true`; senão retorna `false` (chamador segue caminho direto/OpenGate).

> **Nota:** este é o task de correção central. Verificação local = **suite nativa verde** (as unidades puras não regridem) + `trunk fmt`; a corretude do glue é confirmada por revisão + build de firmware no CI. Não há teste nativo do módulo (depende de Arduino/rádio).

- [ ] **Step 1: Declara o helper (privado) no header**

Em `IrrigationModule.h`, junto aos helpers privados de gateway (procure a seção `private:` com `gwSendValveCmd`):

```cpp
    // Fase 7b: ponto único de decisão de roteamento de open/close de UMA zona.
    // Zona de grupo hidráulico => motor (setDesired); zona livre => false (caminho atual).
    bool routeZoneToGroup(uint8_t zoneId, bool open, uint16_t durationS);
```

- [ ] **Step 2: Implementa o helper**

Em `IrrigationModule.cpp` (perto dos demais helpers de gateway, ex.: antes de `gwTick`):

```cpp
bool IrrigationModule::routeZoneToGroup(uint8_t zoneId, bool open, uint16_t durationS)
{
    const HydraulicGroup *hg = gateway.groups.byZone(zoneId);
    if (!hg)
        return false; // zona livre — chamador segue caminho direto/OpenGate
    if (open) {
        // Intertravamento: zona bloqueada não entra no desejado (mesma política do scheduler).
        if (gateway.interlockEngine.zoneVerdict(zoneId).bloqueada) {
            const Zone *z = gateway.zones.byId(zoneId);
            auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::CMD_REJEITADO, zoneId,
                       AuditResult::NACK, z ? z->node : 0);
        } else {
            gateway.groupEngine.setDesired(hg->id, zoneId, true, durationS);
        }
    } else {
        gateway.groupEngine.setDesired(hg->id, zoneId, false, 0);
    }
    return true;
}
```

- [ ] **Step 3: Rewire do scheduler (`gwTick`)**

Substitui o bloco `const HydraulicGroup *hg = gateway.groups.byZone(a.zoneId); if (hg) { … continue; }` (linhas ~1996-2013) por uma chamada ao helper. O `dur` para OPEN é calculado **antes** (clamp por `maxMin`). Resultado:

```cpp
            // --- Fase 7a/7b: zonas de grupo são orquestradas pelo motor (roteamento único) ---
            if (a.type == SchedAction::Type::OPEN) {
                uint16_t durG = a.durationS;
                if (z->maxMin > 0 && durG > (uint16_t)(z->maxMin * 60))
                    durG = (uint16_t)(z->maxMin * 60);
                if (routeZoneToGroup(a.zoneId, true, durG))
                    continue;
            } else { // CLOSE
                if (routeZoneToGroup(a.zoneId, false, 0))
                    continue;
            }
```

(Mantém o restante do corpo do `for` — supressão de espelho e caminho `OpenGate` — intacto para zonas livres.)

- [ ] **Step 4: Rewire de `handleWebCommand` (comando manual do painel)**

No `K::OPEN || K::PULSE_TEST` (após o clamp de `dur`, antes de `gwSendValveCmd`, ~linha 1766):

```cpp
        // Fase 7b: zona de grupo vai pelo motor (bomba + coreografia), não válvula direta.
        if (routeZoneToGroup(z->id, true, dur))
            return true;
        gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts);
        return true;
```

No `K::CLOSE` (~linha 1769):

```cpp
    if (c.kind == K::CLOSE) {
        if (routeZoneToGroup(z->id, false, 0))
            return true;
        gateway.openGate.release(z->id);
        gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
        return true;
    }
```

- [ ] **Step 5: Rewire de `portalRunNetCommand` (gateway-local)**

No ramo `c.action == 1` (após clamp de `dur`, antes de `gwSendValveCmd`, ~linha 1838):

```cpp
            if (routeZoneToGroup(z->id, true, dur)) {
                auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::ABRIR, c.zoneId, AuditResult::OK);
                return true;
            }
            gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts);
```

No `else` (fechar, ~linha 1840):

```cpp
        } else {
            if (routeZoneToGroup(z->id, false, 0)) {
                auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::FECHAR, c.zoneId, AuditResult::OK);
                return true;
            }
            gateway.openGate.release(z->id);
            gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
        }
```

- [ ] **Step 6: Suite nativa não regride**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN), 56 suites — as unidades puras seguem verdes; o módulo não é compilado no native.

- [ ] **Step 7: fmt + commit**

```bash
trunk fmt src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "fix(irrigation): rotear open/close de zona de grupo pelo motor em todos os caminhos (fase 7b)"
```

---

## Task 6: Métodos de aplicação do módulo (`gwApplyGroupUpsert`/`Delete`, `gwRunGroupCommand`)

**Files:**
- Modify: `src/modules/irrigation/IrrigationModule.h` (declarações públicas)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (implementações, junto de `gwApplyInterlockUpsert` ~1644)

**Interfaces:**
- Consumes: `validateGroupZones` (Task 3), `saveGroups()` (existente), `HydraulicGroupTable::upsert/removeById/byId` (existente), `HydraulicGroupEngine::setDesired` (existente).
- Produces:
  - `bool gwApplyGroupUpsert(HydraulicGroup &g, char *err, size_t errCap);` — valida via `validateGroupZones`; se `g.id==0` aloca 1º id livre 1..8; `upsert` + `saveGroups()`. `false` + `err` em falha.
  - `bool gwApplyGroupDelete(uint8_t id);` — `removeById` + `saveGroups()`.
  - `bool gwRunGroupCommand(uint8_t id, bool open, uint16_t durationS);` — `setDesired` em cada membro.

- [ ] **Step 1: Declarações no header**

Em `IrrigationModule.h`, após `gwApplyInterlockDelete` (~linha 85):

```cpp
    // Fase 7b: grupos hidráulicos (painel). Espelham gwApplyInterlock*.
    bool gwApplyGroupUpsert(HydraulicGroup &g, char *err, size_t errCap); // valida + aloca id se 0 + saveGroups()
    bool gwApplyGroupDelete(uint8_t id);                                  // removeById + saveGroups()
    bool gwRunGroupCommand(uint8_t id, bool open, uint16_t durationS);    // controle manual do grupo
```

Garanta que `IrrigationModule.h` inclui `IrrigationWebApi.h` (já inclui — `WebCommand` é usado em `gwRunCommand`) para enxergar `IrrigationWeb::validateGroupZones`.

- [ ] **Step 2: Implementa**

Em `IrrigationModule.cpp`, após `gwApplyInterlockDelete` (~1659):

```cpp
bool IrrigationModule::gwApplyGroupUpsert(HydraulicGroup &g, char *err, size_t errCap)
{
    if (!IrrigationWeb::validateGroupZones(g, gateway.zones, err, errCap))
        return false; // err preenchido
    if (g.id == 0) {
        // Aloca 1º id livre 1..MAX.
        uint8_t freeId = 0;
        for (uint8_t cand = 1; cand <= HydraulicGroupTable::MAX; cand++) {
            if (!gateway.groups.byId(cand)) { freeId = cand; break; }
        }
        if (freeId == 0) {
            snprintf(err, errCap, "tabela de grupos cheia");
            return false;
        }
        g.id = freeId;
    }
    if (!gateway.groups.upsert(g)) {
        snprintf(err, errCap, "grupo invalido (zona em outro grupo?)");
        return false;
    }
    saveGroups();
    return true;
}

bool IrrigationModule::gwApplyGroupDelete(uint8_t id)
{
    if (!gateway.groups.removeById(id))
        return false;
    saveGroups();
    return true;
}

bool IrrigationModule::gwRunGroupCommand(uint8_t id, bool open, uint16_t durationS)
{
    const HydraulicGroup *g = gateway.groups.byId(id);
    if (!g)
        return false;
    uint16_t dur = durationS;
    if (open && dur == 0) {
        // Default: maior maxMin (s) entre os membros; fallback 600 s.
        uint16_t best = 0;
        for (uint8_t i = 0; i < g->zoneCount && i < 8; i++) {
            const Zone *z = gateway.zones.byId(g->zoneIds[i]);
            if (z && z->maxMin > 0 && (uint16_t)(z->maxMin * 60) > best)
                best = (uint16_t)(z->maxMin * 60);
        }
        dur = best ? best : 600;
    }
    if (open && dur > HydraulicGroupEngine::PUMP_CEILING_S)
        dur = HydraulicGroupEngine::PUMP_CEILING_S;
    for (uint8_t i = 0; i < g->zoneCount && i < 8; i++) {
        uint8_t zid = g->zoneIds[i];
        if (open) {
            if (gateway.interlockEngine.zoneVerdict(zid).bloqueada)
                continue; // pula zona bloqueada
            gateway.groupEngine.setDesired(id, zid, true, dur);
        } else {
            gateway.groupEngine.setDesired(id, zid, false, 0);
        }
    }
    auditEvent(AuditOrigin::PAINEL, open ? AuditAction::ABRIR : AuditAction::FECHAR, id, AuditResult::OK);
    return true;
}
```

- [ ] **Step 3: Suite nativa não regride**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN).

- [ ] **Step 4: fmt + commit**

```bash
trunk fmt src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git add src/modules/irrigation/IrrigationModule.h src/modules/irrigation/IrrigationModule.cpp
git commit -m "feat(irrigation): grupo — gwApplyGroupUpsert/Delete + gwRunGroupCommand (fase 7b)"
```

---

## Task 7: Endpoints HTTP (`hGroups*`) + registro

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (handlers + `registerIrrigationHandlers`)

**Interfaces:**
- Consumes: `buildGroups`, `buildGroupsStatus`, `GroupStatusView`, `groupStateLabel`, `parseGroupUpsert/Delete/Command` (Tasks 2-4); `gwApplyGroupUpsert/Delete`, `gwRunGroupCommand`, `gwState().groups`, `gwState().groupEngine` (Tasks 1,6).

> **Nota:** bloco CI-only (compila só em ARCH_ESP32 com webserver). Sem teste nativo; verificação = build CI + revisão. Segue **exatamente** os helpers existentes (`gwReady`, `readBody`, `sendJson`, `sendParseErrors`).

- [ ] **Step 1: Adiciona os handlers**

Em `IrrigationWebEndpoints.cpp`, antes de `registerIrrigationHandlers` (~linha 554):

```cpp
// GET /api/irrigation/groups — lista de config dos grupos
static void hGroupsGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    char buf[2048];
    size_t n = buildGroups(irrigationModule->gwState().groups, buf, sizeof(buf));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// GET /api/irrigation/groups/status — status ao vivo por grupo
static void hGroupsStatus(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    const IrrigationGateway &g = irrigationModule->gwState();

    GroupStatusView views[HydraulicGroupTable::MAX];
    size_t nv = 0;
    for (size_t i = 0; i < g.groups.count() && nv < HydraulicGroupTable::MAX; i++) {
        const HydraulicGroup *grp = g.groups.groupAt(i);
        if (!grp) continue;
        GroupStatusView &v = views[nv++];
        v.id = grp->id;
        v.name = grp->name;
        v.state = (uint8_t)g.groupEngine.stateOf(grp->id);
        v.pump = g.groupEngine.pumpOn(grp->id);
        v.curZone = g.groupEngine.currentZone(grp->id);
        v.openCount = g.groupEngine.openConfirmedCount(grp->id);
    }
    char buf[2048];
    size_t n = buildGroupsStatus(views, nv, buf, sizeof(buf));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// POST /api/irrigation/groups — upsert (id=0 => servidor aloca)
static void hGroupsPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[768];
    size_t nb = readBody(req, body, sizeof(body));
    HydraulicGroup grp;
    ParseResult pr = parseGroupUpsert(body, nb, grp);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    char err[48] = {0};
    if (!irrigationModule->gwApplyGroupUpsert(grp, err, sizeof(err))) {
        char out[128];
        JsonWriter w(out, sizeof(out));
        w.beginObject(); w.key("errors"); w.beginArray(); w.str(err[0] ? err : "erro"); w.endArray(); w.endObject();
        w.done();
        sendJson(res, out, 400);
        return;
    }
    char out[2048];
    size_t n = buildGroups(irrigationModule->gwState().groups, out, sizeof(out));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, out);
}

// POST /api/irrigation/groups/delete — remove por id
static void hGroupsDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0;
    ParseResult pr = parseGroupDelete(body, nb, id);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->gwApplyGroupDelete(id)) {
        sendJson(res, "{\"errors\":[\"grupo inexistente\"]}", 400);
        return;
    }
    char out[2048];
    size_t n = buildGroups(irrigationModule->gwState().groups, out, sizeof(out));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, out);
}

// POST /api/irrigation/groups/command — controle manual do grupo
static void hGroupsCommand(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0; bool open = false; uint16_t dur = 0;
    ParseResult pr = parseGroupCommand(body, nb, id, open, dur);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->gwRunGroupCommand(id, open, dur)) {
        sendJson(res, "{\"errors\":[\"grupo inexistente\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}
```

- [ ] **Step 2: Registra as rotas**

Em `registerIrrigationHandlers`, após a linha de `/maint`:

```cpp
    // Fase 7b: grupos hidráulicos
    server->registerNode(new ResourceNode("/api/irrigation/groups", "GET", &hGroupsGet));
    server->registerNode(new ResourceNode("/api/irrigation/groups/status", "GET", &hGroupsStatus));
    server->registerNode(new ResourceNode("/api/irrigation/groups", "POST", &hGroupsPost));
    server->registerNode(new ResourceNode("/api/irrigation/groups/delete", "POST", &hGroupsDelete));
    server->registerNode(new ResourceNode("/api/irrigation/groups/command", "POST", &hGroupsCommand));
```

- [ ] **Step 3: Suite nativa não regride** (o arquivo é excluído no native, mas confirma que nada mais quebrou)

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN).

- [ ] **Step 4: fmt + commit**

```bash
trunk fmt src/modules/irrigation/IrrigationWebEndpoints.cpp
git add src/modules/irrigation/IrrigationWebEndpoints.cpp
git commit -m "feat(irrigation): grupo — endpoints HTTP /groups[/status|/delete|/command] (fase 7b)"
```

---

## Task 8: Frontend — aba "Grupos"

**Files:**
- Modify: `data/irrigacao/index.html` (botão da aba)
- Modify: `data/irrigacao/app.js` (`renderGrupos`, `groupForm`, `pollGroupStatus`, `RENDER`, `show`)
- Modify: `data/irrigacao/style.css` (badge de estado — pequeno)

**Interfaces:**
- Consome os endpoints da Task 7 via `getJson('/groups')`, `getJson('/groups/status')`, `postJson('/groups', …)`, `postJson('/groups/delete', …)`, `postJson('/groups/command', …)`.

> **Nota:** verificação = revisão visual (servir `data/irrigacao/` localmente ou inspeção) + build CI. Segue o padrão de `renderIntertravamentos`/`interlockForm`.

- [ ] **Step 1: Botão da aba**

Em `data/irrigacao/index.html`, após a aba `programs` (linha 22):

```html
      <button data-tab="grupos" class="tab">Grupos</button>
```

- [ ] **Step 2: `renderGrupos` + `pollGroupStatus`**

Em `data/irrigacao/app.js`, antes do bloco `const RENDER = {` (~linha 1160). Estados → cor de badge:

```js
const GROUP_STATE_CLASS = {
  ocioso: 'gray', abrindo: 'amber', aguardando_partida: 'amber', partindo_bomba: 'amber',
  rodando: 'green', transicao: 'amber', parando_bomba: 'amber', drenando: 'amber',
  fechando: 'amber', adiado: 'red', desconhecido: 'gray',
};

function groupStatusById(list, id) {
  return (Array.isArray(list) ? list : []).find((s) => s && num(s.id) === num(id)) || {};
}

async function renderGrupos() {
  const [groups, status, zones] = await Promise.all([
    getJson('/groups'),
    getJson('/groups/status').catch(() => []),
    getJson('/zones').catch(() => []),
  ]);
  const rows = Array.isArray(groups) ? groups : [];
  const st = Array.isArray(status) ? status : [];
  const zs = Array.isArray(zones) ? zones : [];

  const cards = rows.map((g) => {
    g = g || {};
    const s = groupStatusById(st, g.id);
    const estado = s.estado || 'ocioso';
    const cls = GROUP_STATE_CLASS[estado] || 'gray';
    const bomba = s.bomba ? '<span class="chip green">bomba on</span>' : '<span class="chip gray">bomba off</span>';
    const membros = Array.isArray(g.zonas) ? g.zonas.join(', ') : '';
    return `<div class="card">
      <div class="itl-row">
        <div class="itl-info">
          <div class="name">${esc(g.nome || ('Grupo ' + num(g.id)))}
            <span class="chip ${cls}" data-gstate="${num(g.id)}">${esc(estado)}</span></div>
          <div class="sub" data-gbomba="${num(g.id)}">${bomba} · abertas <span data-gopen="${num(g.id)}">${num(s.abertas)}</span></div>
          <div class="sub">Bomba zona ${num(g.bombaZoneId)} · Zonas: ${esc(membros)} · min ${num(g.minOpen)}/max ${num(g.maxOpen)}</div>
        </div>
        <div class="zbtns">
          <button class="btn outline sm" data-gopenbtn="${num(g.id)}">Abrir</button>
          <button class="btn ghost sm" data-gclosebtn="${num(g.id)}">Fechar</button>
          <button class="btn ghost sm" data-gedit="${num(g.id)}">Editar</button>
          <button class="btn dangerline sm" data-gdel="${num(g.id)}">Excluir</button>
        </div>
      </div>
    </div>`;
  }).join('');

  view.innerHTML =
    `<button class="btn dashed" data-gnew>+ Novo grupo</button>` +
    (cards || '<div class="empty">Nenhum grupo hidráulico.</div>');

  view.querySelector('[data-gnew]').addEventListener('click', () => groupForm(null, zs, rows));
  view.querySelectorAll('[data-gedit]').forEach((b) => b.addEventListener('click', () => {
    groupForm(rows.find((x) => x && num(x.id) === num(b.dataset.gedit)) || null, zs, rows);
  }));
  view.querySelectorAll('[data-gdel]').forEach((b) => b.addEventListener('click', async () => {
    if (!confirm('Excluir este grupo?')) return;
    const r = await postJson('/groups/delete', { id: num(b.dataset.gdel) });
    if (r.ok) renderGrupos().catch(() => {});
    else alert('Falha ao excluir: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }));
  view.querySelectorAll('[data-gopenbtn]').forEach((b) => b.addEventListener('click', async () => {
    const r = await postJson('/groups/command', { id: num(b.dataset.gopenbtn), acao: 'abrir' });
    if (!r.ok) alert('Falha: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }));
  view.querySelectorAll('[data-gclosebtn]').forEach((b) => b.addEventListener('click', async () => {
    const r = await postJson('/groups/command', { id: num(b.dataset.gclosebtn), acao: 'fechar' });
    if (!r.ok) alert('Falha: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }));
}

// Atualiza só os badges/contadores ao vivo (não recria a lista; no-op se a aba/lista não está montada).
async function pollGroupStatus() {
  if (!document.querySelector('[data-gstate]')) return; // form aberto ou outra aba
  const st = await getJson('/groups/status').catch(() => []);
  (Array.isArray(st) ? st : []).forEach((s) => {
    if (!s) return;
    const badge = view.querySelector(`[data-gstate="${num(s.id)}"]`);
    if (badge) {
      const cls = GROUP_STATE_CLASS[s.estado] || 'gray';
      badge.className = 'chip ' + cls;
      badge.textContent = s.estado || 'ocioso';
    }
    const open = view.querySelector(`[data-gopen="${num(s.id)}"]`);
    if (open) open.textContent = num(s.abertas);
    const bombaWrap = view.querySelector(`[data-gbomba="${num(s.id)}"]`);
    if (bombaWrap) {
      bombaWrap.innerHTML = (s.bomba ? '<span class="chip green">bomba on</span>' : '<span class="chip gray">bomba off</span>') +
        ` · abertas <span data-gopen="${num(s.id)}">${num(s.abertas)}</span>`;
    }
  });
}
```

- [ ] **Step 3: `groupForm`**

Ainda em `app.js`, após `renderGrupos` (segue a forma de `interlockForm`: estado local `st`, `render()`, `save()`). Zonas de espelho (`fonteInput >= 0`) são excluídas do multiselect:

```js
function groupForm(group, zones, allGroups) {
  const editing = !!group;
  const zs = (Array.isArray(zones) ? zones : []).filter((z) => z && num(z.fonteInput ?? -1) < 0);
  const st = group ? JSON.parse(JSON.stringify(group)) : {
    id: 0, nome: '', bombaZoneId: 0, zonas: [], minOpen: 1, maxOpen: 1, transicao: 0,
    overlapS: 10, startAfterOpenS: 5, stopBeforeCloseS: 8, minRunMin: 5, maxStartsHour: 6,
  };
  st.zonas = Array.isArray(st.zonas) ? st.zonas : [];

  function render() {
    const zoneChips = zs.map((z) => {
      const on = st.zonas.includes(num(z.id));
      return `<button class="chip ${on ? 'green' : 'gray'}" data-gz="${num(z.id)}">${num(z.id)}</button>`;
    }).join(' ');
    const bombaOpts = `<option value="0">— sem bomba —</option>` +
      zs.map((z) => `<option value="${num(z.id)}" ${num(st.bombaZoneId) === num(z.id) ? 'selected' : ''}>zona ${num(z.id)}</option>`).join('');

    view.innerHTML = `
      <div class="form">
        <label class="fld"><span class="flbl">Nome</span>
          <input class="finput" id="g-nome" maxlength="15" value="${esc(st.nome || '')}"></label>
        <label class="fld"><span class="flbl">Bomba (zona GPO)</span>
          <select class="finput" id="g-bomba">${bombaOpts}</select></label>
        <div class="fld"><span class="flbl">Zonas membro</span><div class="chips">${zoneChips || '<span class="sub">Sem zonas não-espelho.</span>'}</div></div>
        <div class="frow">
          <label class="fld"><span class="flbl">Mín. abertas</span><input class="finput" id="g-min" type="number" min="1" value="${num(st.minOpen)}"></label>
          <label class="fld"><span class="flbl">Máx. abertas (0=sem teto)</span><input class="finput" id="g-max" type="number" min="0" value="${num(st.maxOpen)}"></label>
        </div>
        <label class="fld"><span class="flbl">Transição</span>
          <select class="finput" id="g-trans">
            <option value="0" ${num(st.transicao) === 0 ? 'selected' : ''}>abrir antes de fechar</option>
            <option value="1" ${num(st.transicao) === 1 ? 'selected' : ''}>fechar antes de abrir</option>
          </select></label>
        <div class="frow">
          <label class="fld"><span class="flbl">Sobrepos. (s)</span><input class="finput" id="g-ov" type="number" min="0" value="${num(st.overlapS)}"></label>
          <label class="fld"><span class="flbl">Partida após abrir (s)</span><input class="finput" id="g-sa" type="number" min="0" value="${num(st.startAfterOpenS)}"></label>
        </div>
        <div class="frow">
          <label class="fld"><span class="flbl">Parar antes fechar (s)</span><input class="finput" id="g-sb" type="number" min="0" value="${num(st.stopBeforeCloseS)}"></label>
          <label class="fld"><span class="flbl">Func. mín. (min)</span><input class="finput" id="g-mr" type="number" min="0" value="${num(st.minRunMin)}"></label>
        </div>
        <label class="fld"><span class="flbl">Máx. partidas/hora</span><input class="finput" id="g-ms" type="number" min="0" value="${num(st.maxStartsHour)}"></label>
        <div class="frow">
          <button class="btn" data-gsave>${editing ? 'Salvar' : 'Criar'}</button>
          <button class="btn ghost" data-gback>Cancelar</button>
        </div>
      </div>`;

    view.querySelectorAll('[data-gz]').forEach((b) => b.addEventListener('click', () => {
      const zid = num(b.dataset.gz);
      const i = st.zonas.indexOf(zid);
      if (i >= 0) st.zonas.splice(i, 1); else st.zonas.push(zid);
      render();
    }));
    view.querySelector('[data-gback]').addEventListener('click', () => renderGrupos().catch(() => {}));
    view.querySelector('[data-gsave]').addEventListener('click', save);
  }

  async function save() {
    const body = {
      id: num(st.id),
      nome: view.querySelector('#g-nome').value.slice(0, 15),
      bombaZoneId: num(view.querySelector('#g-bomba').value),
      zonas: st.zonas,
      minOpen: num(view.querySelector('#g-min').value),
      maxOpen: num(view.querySelector('#g-max').value),
      transicao: num(view.querySelector('#g-trans').value),
      overlapS: num(view.querySelector('#g-ov').value),
      startAfterOpenS: num(view.querySelector('#g-sa').value),
      stopBeforeCloseS: num(view.querySelector('#g-sb').value),
      minRunMin: num(view.querySelector('#g-mr').value),
      maxStartsHour: num(view.querySelector('#g-ms').value),
    };
    if (!body.zonas.length) { alert('Selecione ao menos uma zona.'); return; }
    const r = await postJson('/groups', body);
    if (r.ok) renderGrupos().catch(() => {});
    else alert('Falha: ' + ((r.body && Array.isArray(r.body.errors)) ? r.body.errors.join(', ') : 'erro'));
  }

  render();
}
```

- [ ] **Step 4: Registra no `RENDER` e no refresh**

No objeto `RENDER` (~1161), adiciona:

```js
  grupos: renderGrupos,
```

Em `show(tab)`, troca o bloco de refresh (~1190) para incluir grupos com o poller leve:

```js
  if (tab === 'overview' || tab === 'stations' || tab === 'sensores') {
    timer = setInterval(() => fn().catch(() => {}), 3000);
  } else if (tab === 'grupos') {
    timer = setInterval(() => pollGroupStatus().catch(() => {}), 3000);
  }
```

- [ ] **Step 5: Badge cinza no CSS (se ausente)**

Em `data/irrigacao/style.css`, confirme que `.chip.gray` e `.chip.amber` existem; se não, adicione junto às demais cores de `.chip`:

```css
.chip.gray { background: #33373d; color: #c7ccd1; }
.chip.amber { background: #7a5a10; color: #ffd98a; }
```

- [ ] **Step 6: fmt + commit**

```bash
trunk fmt data/irrigacao/app.js data/irrigacao/index.html data/irrigacao/style.css
git add data/irrigacao/app.js data/irrigacao/index.html data/irrigacao/style.css
git commit -m "feat(irrigation): grupo — aba Grupos no painel (CRUD + status ao vivo + comando) (fase 7b)"
```

---

## Task 9: Roadmap + verificação final da suite

**Files:**
- Modify: `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md` (linha 13, Fase 7)

- [ ] **Step 1: Marca 7b concluída no roadmap**

Na linha da Fase 7, acrescenta ao final da célula do plano:

```
**7b (painel/roteamento) concluída** (2026-07-24, spec 2026-07-24-irrigacao-fase7b-grupos-web-design.md, plano 2026-07-24-irrigacao-fase7b-grupos-web.md): endpoints /groups[/status|/delete|/command] + aba "Grupos" (CRUD + status ao vivo + controle manual); validação de zona-membro (rejeita espelho fonteInput>=0, zona inexistente, bomba-membro) via validateGroupZones puro; roteamento único routeZoneToGroup (grupo isento do cap global IL_SIMULTANEIDADE — Opção A; corrige open-direto-sem-bomba nos caminhos manual/portal); create id=0 aloca id livre no servidor. Export/backup de grupos ADIADO p/ Fase 8. Suite nativa 56 (casos em test_irrigation_webapi + test_hydraulic_group_engine).
```

- [ ] **Step 2: Roda a suite nativa inteira**

Run: `./bin/run-tests.sh`
Expected: exit 0 (GREEN). Confirma que `test/native-suite-count` permanece **56** e que todos os casos novos de grupo passam.

- [ ] **Step 3: Confirma o formato**

Run: `trunk fmt`
Expected: sem alterações pendentes (tudo já formatado nos commits anteriores).

- [ ] **Step 4: commit**

```bash
git add docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md
git commit -m "docs(irrigation): marca Fase 7b (painel de grupos) concluida no roadmap"
```

- [ ] **Step 5: Push para o fork** (usuário trabalha de múltiplas máquinas)

```bash
git push fork sistema-irrigacao
```

---

## Self-Review (preenchido na escrita do plano)

**Cobertura da spec:**
- §3 arquitetura/layering → Tasks 2-4 (puro), 5-6 (módulo), 7 (endpoints), 8 (front). ✓
- §4 endpoints (GET /groups, /groups/status, POST /groups, /groups/delete, /groups/command; id=0 aloca) → Task 7 + alloc na Task 6. ✓
- §5 JSON (config, status, mapa de estado) → Task 4 (`buildGroups`/`buildGroupsStatus`/`groupStateLabel`). ✓
- §6 validação estrutural (parser) + semântica (espelho/existência/bomba) → Tasks 2 e 3. ✓
- §7.1 isenção do cap (Opção A) + §7.2 fix de roteamento (helper único, 3 call-sites) → Task 5. ✓
- §7.3 controle manual do grupo → Task 6 (`gwRunGroupCommand`) + Task 7 (endpoint) + Task 8 (botões). ✓
- §8 aba Grupos (cards, badge ao vivo, form, poll) → Task 8. ✓
- §9 fora de escopo (export→F8, portal, pressão) → respeitado; nenhum task o implementa. ✓
- §10 testes → Tasks 1-4 (nativos); glue por revisão/CI. ✓

**Placeholders:** nenhum "TBD/TODO"; todo passo com código concreto. ✓

**Consistência de tipos:** `parseGroupUpsert(…, HydraulicGroup&)`, `validateGroupZones(HydraulicGroup&, ZoneTable&, char*, size_t)`, `gwApplyGroupUpsert(HydraulicGroup&, char*, size_t)`, `GroupStatusView{id,name,state,pump,curZone,openCount}`, `groupStateLabel(uint8_t)` — nomes/assinaturas idênticos entre Tasks 1-8. Rotas `/api/irrigation/groups[...]` idênticas entre Task 7 (registro) e Task 8 (fetch). ✓

---

## Execution Handoff

Escolha o modo de execução ao iniciar (ver rodapé do skill writing-plans).
