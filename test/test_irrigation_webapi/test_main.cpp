#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/HydraulicGroupTable.h"
#include "modules/irrigation/InterlockTable.h"
#include "modules/irrigation/IrrigationWebApi.h"
#include "modules/irrigation/ProgramScheduler.h"
#include <string.h>
#include <unity.h>

using namespace IrrigationWeb;

void setUp(void) {}
void tearDown(void) {}

static bool contains(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }

static void test_computeSync_states()
{
    TEST_ASSERT_EQUAL(SyncState::INALCANCAVEL, computeSync(5, 5, true));
    TEST_ASSERT_EQUAL(SyncState::SINCRONIZADA, computeSync(5, 5, false));
    TEST_ASSERT_EQUAL(SyncState::PENDENTE, computeSync(6, 5, false));
    TEST_ASSERT_EQUAL(SyncState::PENDENTE, computeSync(1, 0, false));
}

static void test_buildOverview_json()
{
    OverviewCtx c = {};
    c.hasRtc = false;
    c.stationCount = 3;
    c.running = 0;
    c.alertCount = 2;
    c.pairingPending = true;
    c.pairingNodeId = 0xABCD;
    c.pairingSecondsLeft = 90;
    char buf[512];
    size_t n = buildOverview(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"hasRtc\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"stationCount\":3"));
    TEST_ASSERT_TRUE(contains(buf, "\"alertCount\":2"));
    TEST_ASSERT_TRUE(contains(buf, "\"pairingPending\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"pairingNodeId\":43981")); // 0xABCD
}

static void test_buildOverview_truncationReturnsZero()
{
    OverviewCtx c = {};
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(0, buildOverview(c, buf, sizeof(buf)));
}

// Arrays de escalares precisam de vírgula entre elementos (str/num/boolean chamam sep_).
static void test_jsonWriter_scalarArrayCommas()
{
    char buf[128];
    JsonWriter w(buf, sizeof(buf));
    w.beginArray();
    w.str("a");
    w.str("b");
    w.endArray();
    TEST_ASSERT_EQUAL_STRING("[\"a\",\"b\"]", buf);

    JsonWriter w2(buf, sizeof(buf));
    w2.beginArray();
    w2.num(1);
    w2.num(2);
    w2.endArray();
    TEST_ASSERT_EQUAL_STRING("[1,2]", buf);
}

// str() escapa aspas/barra/newline e descarta controle.
static void test_jsonWriter_strEscaping()
{
    char buf[64];
    // "\x01" separado de "e" por concatenação: senão \x01e vira um único hex 0x1E (greedy).
    JsonWriter w(buf, sizeof(buf));
    w.str("a\"b\\c\nd\x01"
          "e");
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\nde\"", buf);
}

static void test_buildStations_json()
{
    StationView v[2] = {};
    v[0].node = 0x1111;
    v[0].name = "Pasto Norte";
    v[0].sync = SyncState::SINCRONIZADA;
    v[0].secsSinceHeard = 120;
    v[0].vbatCentiV = 1250;
    v[0].vpanelCentiV = 1800;
    v[0].snrQuarterDb = 40;
    v[0].rebootCount = 2;
    v[0].flags = 0;
    v[0].lat = -2212340;
    v[0].lon = -4765430;
    v[1].node = 0x2222;
    v[1].name = "Horta";
    v[1].sync = SyncState::INALCANCAVEL;
    v[1].secsSinceHeard = 4000;
    char buf[1024];
    size_t n = buildStations(v, 2, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"node\":4369")); // 0x1111
    TEST_ASSERT_TRUE(contains(buf, "\"name\":\"Pasto Norte\""));
    TEST_ASSERT_TRUE(contains(buf, "\"sync\":\"sincronizada\""));
    TEST_ASSERT_TRUE(contains(buf, "\"sync\":\"inalcancavel\""));
    TEST_ASSERT_TRUE(contains(buf, "\"vbatCentiV\":1250"));
    TEST_ASSERT_TRUE(contains(buf, "\"lat\":-2212340"));
}

static void test_buildZones_json()
{
    ZoneTable t;
    Zone z;
    z.id = 1;
    snprintf(z.name, sizeof(z.name), "Horta");
    z.node = 0x1111;
    z.tipo = 0;
    z.index = 2;
    z.maxMin = 45;
    z.padraoMin = 20;
    z.fonteInput = -1;
    TEST_ASSERT_TRUE(t.upsert(z));
    char buf[1024];
    size_t n = buildZones(t, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"name\":\"Horta\""));
    TEST_ASSERT_TRUE(contains(buf, "\"node\":4369"));
    TEST_ASSERT_TRUE(contains(buf, "\"tipo\":0"));
    TEST_ASSERT_TRUE(contains(buf, "\"index\":2"));
    TEST_ASSERT_TRUE(contains(buf, "\"maxMin\":45"));
    TEST_ASSERT_TRUE(contains(buf, "\"fonteInput\":-1"));
}

static void test_buildPrograms_json()
{
    ProgramScheduler s;
    Program p;
    p.id = 1;
    p.enabled = true;
    p.daysMask = 0x7F;
    p.startMinute = 360;
    p.stepCount = 1;
    p.steps[0].zoneId = 1;
    p.steps[0].durationMin = 15;
    TEST_ASSERT_TRUE(s.upsert(p));
    char buf[1024];
    size_t n = buildPrograms(s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"enabled\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"daysMask\":127"));
    TEST_ASSERT_TRUE(contains(buf, "\"startMinute\":360"));
    TEST_ASSERT_TRUE(contains(buf, "\"zoneId\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"durationMin\":15"));
}

static void test_parseZoneUpsert_ok()
{
    const char *j = "{\"id\":3,\"name\":\"Horta\",\"node\":4369,\"tipo\":0,\"index\":2,\"maxMin\":45,\"padraoMin\":20,\"fonteInput\":-1}";
    Zone z;
    ParseResult r = parseZoneUpsert(j, strlen(j), z);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(3, z.id);
    TEST_ASSERT_EQUAL_STRING("Horta", z.name);
    TEST_ASSERT_EQUAL_HEX32(4369, z.node);
    TEST_ASSERT_EQUAL_UINT16(45, z.maxMin);
    TEST_ASSERT_EQUAL_INT8(-1, z.fonteInput);
}

static void test_parseZoneUpsert_rejectsBadRange()
{
    Zone z;
    const char *j1 = "{\"id\":0,\"name\":\"X\",\"node\":1,\"tipo\":0,\"index\":0,\"maxMin\":10,\"padraoMin\":5}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j1, strlen(j1), z).ok); // id 0
    const char *j2 = "{\"id\":1,\"name\":\"X\",\"node\":1,\"tipo\":0,\"index\":0,\"maxMin\":200,\"padraoMin\":5}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j2, strlen(j2), z).ok); // maxMin > 120
    const char *j3 = "{\"id\":1,\"name\":\"\",\"node\":1,\"tipo\":0,\"index\":0,\"maxMin\":10,\"padraoMin\":5}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j3, strlen(j3), z).ok); // nome vazio
    const char *j4 = "{\"id\":1,\"name\":\"X\",\"node\":0,\"tipo\":0,\"index\":0,\"maxMin\":10,\"padraoMin\":5}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j4, strlen(j4), z).ok); // node 0
    const char *j5 = "{\"id\":1,\"name\":\"X\",\"node\":1,\"tipo\":0,\"index\":0,\"maxMin\":10,\"padraoMin\":50}";
    TEST_ASSERT_FALSE(parseZoneUpsert(j5, strlen(j5), z).ok); // padrao > max
}

static void test_parseZoneDelete_ok()
{
    const char *j = "{\"id\":7}";
    uint8_t id = 0;
    TEST_ASSERT_TRUE(parseZoneDelete(j, strlen(j), id).ok);
    TEST_ASSERT_EQUAL_UINT8(7, id);
    const char *jb = "{\"id\":0}";
    TEST_ASSERT_FALSE(parseZoneDelete(jb, strlen(jb), id).ok);
}

static void test_parseProgramUpsert_ok()
{
    const char *j = "{\"id\":2,\"enabled\":true,\"daysMask\":127,\"startMinute\":360,"
                    "\"steps\":[{\"zoneId\":1,\"durationMin\":15},{\"zoneId\":2,\"durationMin\":20}]}";
    Program p;
    ParseResult r = parseProgramUpsert(j, strlen(j), p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(2, p.id);
    TEST_ASSERT_EQUAL_UINT8(2, p.stepCount);
    TEST_ASSERT_EQUAL_UINT8(1, p.steps[0].zoneId);
    TEST_ASSERT_EQUAL_UINT16(20, p.steps[1].durationMin);
}

static void test_parseProgramUpsert_rejectsTooManySteps()
{
    // 9 etapas > 8
    const char *j = "{\"id\":1,\"daysMask\":1,\"startMinute\":0,\"steps\":["
        "{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},"
        "{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},"
        "{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1},{\"zoneId\":1,\"durationMin\":1}]}";
    Program p;
    TEST_ASSERT_FALSE(parseProgramUpsert(j, strlen(j), p).ok);
}

static void test_parseProgramUpsert_rejectsNoSteps()
{
    const char *j = "{\"id\":1,\"daysMask\":1,\"startMinute\":0,\"steps\":[]}";
    Program p;
    TEST_ASSERT_FALSE(parseProgramUpsert(j, strlen(j), p).ok);
}

// Uma etapa sem durationMin não pode "pegar emprestado" o valor de uma etapa posterior:
// o slice de JsonReader deve respeitar _len e rejeitar.
static void test_parseProgramUpsert_rejectsTruncatedStep()
{
    const char *j = "{\"id\":1,\"daysMask\":1,\"startMinute\":0,\"steps\":[{\"zoneId\":1}]}";
    Program p;
    TEST_ASSERT_FALSE(parseProgramUpsert(j, strlen(j), p).ok);
}

// String sem aspa de fechamento: getStr deve retornar false.
static void test_jsonReader_unterminatedString()
{
    const char *s = "{\"name\":\"abc}";
    JsonReader r(s, strlen(s));
    char name[16] = {0};
    TEST_ASSERT_FALSE(r.getStr("name", name, sizeof(name)));
}

// Valor ausente após ':' — getInt deve retornar false (sem dígitos).
static void test_jsonReader_missingValue()
{
    const char *s = "{\"id\":}";
    JsonReader r(s, strlen(s));
    int64_t v = 0;
    TEST_ASSERT_FALSE(r.getInt("id", v));
}

// _len é honrado (não o NUL): "id":5 está APÓS len, logo é invisível.
static void test_jsonReader_respectsLen()
{
    const char *s = "{\"x\":1} \"id\":5";
    JsonReader r(s, 7); // apenas {"x":1}
    int64_t v = 0;
    TEST_ASSERT_FALSE(r.getInt("id", v)); // "id" está além de len
    TEST_ASSERT_TRUE(r.getInt("x", v));   // "x" está dentro de len
    TEST_ASSERT_EQUAL_INT64(1, v);
}

static void test_parseProgramToggle_ok()
{
    const char *j = "{\"id\":4,\"enabled\":false}";
    uint8_t id = 0; bool en = true;
    TEST_ASSERT_TRUE(parseProgramToggle(j, strlen(j), id, en).ok);
    TEST_ASSERT_EQUAL_UINT8(4, id);
    TEST_ASSERT_FALSE(en);
}

static void test_parseCommand_kinds()
{
    WebCommand c;
    const char *jp = "{\"kind\":\"pulse\",\"zoneId\":1}";
    TEST_ASSERT_TRUE(parseCommand(jp, strlen(jp), c).ok);
    TEST_ASSERT_EQUAL(CmdKind::PULSE_TEST, c.kind);
    TEST_ASSERT_EQUAL_UINT16(10, c.durationS);

    const char *jo = "{\"kind\":\"open\",\"zoneId\":2,\"durationS\":600}";
    TEST_ASSERT_TRUE(parseCommand(jo, strlen(jo), c).ok);
    TEST_ASSERT_EQUAL(CmdKind::OPEN, c.kind);
    TEST_ASSERT_EQUAL_UINT16(600, c.durationS);

    const char *ja = "{\"kind\":\"approve_pairing\"}";
    TEST_ASSERT_TRUE(parseCommand(ja, strlen(ja), c).ok);
    TEST_ASSERT_EQUAL(CmdKind::APPROVE_PAIRING, c.kind);

    const char *jbad = "{\"kind\":\"open\",\"zoneId\":0,\"durationS\":600}";
    TEST_ASSERT_FALSE(parseCommand(jbad, strlen(jbad), c).ok); // zoneId 0
}

// ── buildInterlocks ──────────────────────────────────────────────────────────

static void test_buildInterlocks_basic()
{
    InterlockTable tbl;
    InterlockRule r = {};
    r.id = 1;
    r.tipo = IL_SENSOR;
    r.node = 2712847316u; // uint32 grande
    r.sensorIdx = 2;
    r.condicao = COND_MAIOR_QUE;
    r.valorCenti = 5000;
    r.histereseCenti = 200;
    r.acao = ACAO_FECHAR_E_BLOQUEAR;
    r.zoneIds[0] = 3;
    r.zoneIds[1] = 7;
    r.zoneIds[2] = 0; // fim
    r.todas = false;
    snprintf(r.mensagem, sizeof(r.mensagem), "Chuva forte");
    r.maxAbertas = 0;
    TEST_ASSERT_TRUE(tbl.upsert(r));

    char buf[1024];
    size_t n = buildInterlocks(tbl, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"tipo\":0"));
    TEST_ASSERT_TRUE(contains(buf, "\"node\":2712847316"));
    TEST_ASSERT_TRUE(contains(buf, "\"sensor\":2"));
    TEST_ASSERT_TRUE(contains(buf, "\"condicao\":3")); // COND_MAIOR_QUE=3
    TEST_ASSERT_TRUE(contains(buf, "\"valor\":5000"));
    TEST_ASSERT_TRUE(contains(buf, "\"histerese\":200"));
    TEST_ASSERT_TRUE(contains(buf, "\"acao\":1")); // ACAO_FECHAR_E_BLOQUEAR=1
    TEST_ASSERT_TRUE(contains(buf, "\"zonas\":[3,7]"));
    TEST_ASSERT_TRUE(contains(buf, "\"todas\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"mensagem\":\"Chuva forte\""));
    TEST_ASSERT_TRUE(contains(buf, "\"maxAbertas\":0"));
}

static void test_buildInterlocks_todas_semZonas()
{
    InterlockTable tbl;
    InterlockRule r = {};
    r.id = 5;
    r.tipo = IL_SIMULTANEIDADE;
    r.todas = true;
    r.maxAbertas = 2;
    // zoneIds todos zerados → zonas:[]
    TEST_ASSERT_TRUE(tbl.upsert(r));

    char buf[512];
    size_t n = buildInterlocks(tbl, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"tipo\":1")); // IL_SIMULTANEIDADE
    TEST_ASSERT_TRUE(contains(buf, "\"zonas\":[]"));
    TEST_ASSERT_TRUE(contains(buf, "\"todas\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"maxAbertas\":2"));
}

static void test_buildInterlocks_empty()
{
    InterlockTable tbl; // sem regras
    char buf[64];
    size_t n = buildInterlocks(tbl, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("[]", buf);
}

// ── parseInterlockUpsert ─────────────────────────────────────────────────────

static void test_parseInterlockUpsert_ok()
{
    const char *j =
        "{\"id\":3,\"tipo\":0,\"node\":2712847316,\"sensor\":1,\"condicao\":2,"
        "\"valor\":3500,\"histerese\":100,\"acao\":0,"
        "\"zonas\":[2,5,9],\"todas\":false,\"mensagem\":\"Seco\",\"maxAbertas\":0}";
    InterlockRule out = {};
    ParseResult r = parseInterlockUpsert(j, strlen(j), out);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(3, out.id);
    TEST_ASSERT_EQUAL_UINT8(0, out.tipo); // IL_SENSOR
    TEST_ASSERT_EQUAL_HEX32(2712847316u, out.node);
    TEST_ASSERT_EQUAL_UINT8(1, out.sensorIdx);
    TEST_ASSERT_EQUAL_UINT8(2, out.condicao); // COND_MENOR_QUE
    TEST_ASSERT_EQUAL_INT32(3500, out.valorCenti);
    TEST_ASSERT_EQUAL_UINT16(100, out.histereseCenti);
    TEST_ASSERT_EQUAL_UINT8(0, out.acao); // ACAO_BLOQUEAR_ABERTURA
    TEST_ASSERT_EQUAL_UINT8(2, out.zoneIds[0]);
    TEST_ASSERT_EQUAL_UINT8(5, out.zoneIds[1]);
    TEST_ASSERT_EQUAL_UINT8(9, out.zoneIds[2]);
    TEST_ASSERT_EQUAL_UINT8(0, out.zoneIds[3]); // terminador
    TEST_ASSERT_FALSE(out.todas);
    TEST_ASSERT_EQUAL_STRING("Seco", out.mensagem);
    TEST_ASSERT_EQUAL_UINT8(0, out.maxAbertas);
}

static void test_parseInterlockUpsert_rejectsBadId()
{
    InterlockRule out = {};
    const char *j0 = "{\"id\":0,\"tipo\":0,\"node\":1,\"sensor\":0,\"condicao\":0,"
                     "\"valor\":0,\"histerese\":0,\"acao\":0,\"zonas\":[],\"todas\":false,"
                     "\"mensagem\":\"\",\"maxAbertas\":0}";
    TEST_ASSERT_FALSE(parseInterlockUpsert(j0, strlen(j0), out).ok); // id=0 inválido
}

static void test_parseInterlockUpsert_todas_comZonas()
{
    const char *j =
        "{\"id\":10,\"tipo\":1,\"node\":0,\"sensor\":0,\"condicao\":0,"
        "\"valor\":0,\"histerese\":0,\"acao\":0,"
        "\"zonas\":[],\"todas\":true,\"mensagem\":\"\",\"maxAbertas\":3}";
    InterlockRule out = {};
    ParseResult r = parseInterlockUpsert(j, strlen(j), out);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(10, out.id);
    TEST_ASSERT_TRUE(out.todas);
    TEST_ASSERT_EQUAL_UINT8(3, out.maxAbertas);
    TEST_ASSERT_EQUAL_UINT8(0, out.zoneIds[0]); // array vazio
}

// zonas[] com 10 elementos: o guard zCount<8 deve descartar os dois últimos.
static void test_parseInterlockUpsert_zonasOverflowGuard()
{
    const char *j =
        "{\"id\":1,\"tipo\":0,\"node\":1,\"sensor\":0,\"condicao\":0,"
        "\"valor\":0,\"histerese\":0,\"acao\":0,"
        "\"zonas\":[1,2,3,4,5,6,7,8,9,10],\"todas\":false,\"mensagem\":\"\",\"maxAbertas\":0}";
    InterlockRule out = {};
    ParseResult pr = parseInterlockUpsert(j, strlen(j), out);
    TEST_ASSERT_TRUE(pr.ok);
    // Os 8 primeiros devem ter chegado intactos
    TEST_ASSERT_EQUAL_UINT8(1, out.zoneIds[0]);
    TEST_ASSERT_EQUAL_UINT8(8, out.zoneIds[7]);
    // O 9º slot seria zoneIds[8], mas o array tem só 8 entradas; validamos
    // indiretamente: os 8 slots preenchidos cobrem 1..8, nada além.
}

// "zonas" ausente: parse deve ter ok e zoneIds[0]==0 (lista vazia).
static void test_parseInterlockUpsert_zonasAusente()
{
    const char *j =
        "{\"id\":2,\"tipo\":1,\"node\":0,\"sensor\":0,\"condicao\":0,"
        "\"valor\":0,\"histerese\":0,\"acao\":0,"
        "\"todas\":false,\"mensagem\":\"\",\"maxAbertas\":2}";
    InterlockRule out = {};
    ParseResult pr = parseInterlockUpsert(j, strlen(j), out);
    TEST_ASSERT_TRUE(pr.ok);
    TEST_ASSERT_EQUAL_UINT8(0, out.zoneIds[0]); // lista vazia — nenhum id foi escrito
}

// ── parseInterlockDelete ─────────────────────────────────────────────────────

static void test_parseInterlockDelete_ok()
{
    uint8_t id = 0;
    const char *j = "{\"id\":42}";
    TEST_ASSERT_TRUE(parseInterlockDelete(j, strlen(j), id).ok);
    TEST_ASSERT_EQUAL_UINT8(42, id);
}

static void test_parseInterlockDelete_rejectsBadId()
{
    uint8_t id = 0;
    const char *j = "{\"id\":0}";
    TEST_ASSERT_FALSE(parseInterlockDelete(j, strlen(j), id).ok);
    const char *j256 = "{\"id\":256}"; // > uint8 range para validação de id
    // 256 como int64 → cast para uint8 = 0; deve rejeitar via id<1||id>255
    TEST_ASSERT_FALSE(parseInterlockDelete(j256, strlen(j256), id).ok);
}

// ── buildSensorsGateway ──────────────────────────────────────────────────────

static void test_buildSensorsGateway_basic()
{
    GwStationSensors st[2] = {};
    st[0].node = 0xAABBCCDD;
    st[0].stationName = "Pasto Norte";
    st[0].tamper = true;
    st[0].count = 2;
    st[0].itens[0] = {0, 1, 2500, "Umidade"};
    st[0].itens[1] = {1, 0, 0, ""}; // sem nome

    st[1].node = 0x11223344;
    st[1].stationName = "";
    st[1].tamper = false;
    st[1].count = 1;
    st[1].itens[0] = {3, 2, -100, "Temp"};

    char buf[2048];
    size_t n = buildSensorsGateway(st, 2, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    // estação 0
    TEST_ASSERT_TRUE(contains(buf, "\"node\":2864434397")); // 0xAABBCCDD
    TEST_ASSERT_TRUE(contains(buf, "\"nome\":\"Pasto Norte\""));
    TEST_ASSERT_TRUE(contains(buf, "\"tamper\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"sensores\":["));
    TEST_ASSERT_TRUE(contains(buf, "\"idx\":0"));
    TEST_ASSERT_TRUE(contains(buf, "\"tipo\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"valor\":2500"));
    TEST_ASSERT_TRUE(contains(buf, "\"nome\":\"Umidade\""));
    // estação 1
    TEST_ASSERT_TRUE(contains(buf, "\"tamper\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"valor\":-100"));
    TEST_ASSERT_TRUE(contains(buf, "\"nome\":\"Temp\""));
}

static void test_buildSensorsGateway_empty()
{
    char buf[32];
    size_t n = buildSensorsGateway(nullptr, 0, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("[]", buf);
}

// ── parseMaintWindow ─────────────────────────────────────────────────────────

static void test_parseMaintWindow_ok()
{
    const char *j = "{\"node\":12345,\"minutes\":30}";
    uint32_t node = 0;
    uint16_t minutes = 0;
    TEST_ASSERT_TRUE(parseMaintWindow(j, strlen(j), node, minutes).ok);
    TEST_ASSERT_EQUAL_HEX32(12345u, node);
    TEST_ASSERT_EQUAL_UINT16(30, minutes);
}

static void test_parseMaintWindow_zero_ok()
{
    const char *j = "{\"node\":1,\"minutes\":0}";
    uint32_t node = 0;
    uint16_t minutes = 99;
    TEST_ASSERT_TRUE(parseMaintWindow(j, strlen(j), node, minutes).ok);
    TEST_ASSERT_EQUAL_UINT16(0, minutes);
}

static void test_parseMaintWindow_clampMaxMinutes()
{
    // minutes > 1440 deve falhar
    const char *j = "{\"node\":1,\"minutes\":1441}";
    uint32_t node = 0;
    uint16_t minutes = 0;
    TEST_ASSERT_FALSE(parseMaintWindow(j, strlen(j), node, minutes).ok);
}

static void test_parseMaintWindow_maxBoundaryAccepted()
{
    // minutes == 1440 é o limite máximo inclusivo
    const char *j = "{\"node\":123,\"minutes\":1440}";
    uint32_t node = 0;
    uint16_t minutes = 0;
    ParseResult pr = parseMaintWindow(j, strlen(j), node, minutes);
    TEST_ASSERT_TRUE(pr.ok);
    TEST_ASSERT_EQUAL_UINT16(1440, minutes);
}

// ── parseSensorName ──────────────────────────────────────────────────────────

static void test_parseSensorName_ok()
{
    const char *j = "{\"node\":2712847316,\"sensor\":2,\"nome\":\"Umidade\"}";
    uint32_t node = 0;
    uint8_t idx = 0;
    char name[16] = {0};
    ParseResult r = parseSensorName(j, strlen(j), node, idx, name, sizeof(name));
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_HEX32(2712847316u, node);
    TEST_ASSERT_EQUAL_UINT8(2, idx);
    TEST_ASSERT_EQUAL_STRING("Umidade", name);
}

static void test_parseSensorName_truncatesLongName()
{
    // "AbcdefghijklmnopÇ" tem > 15 chars úteis
    const char *j = "{\"node\":1,\"sensor\":0,\"nome\":\"AbcdefghijklmnopX\"}";
    uint32_t node = 0;
    uint8_t idx = 0;
    char name[16] = {0};
    ParseResult r = parseSensorName(j, strlen(j), node, idx, name, sizeof(name));
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(15, strlen(name)); // cap-1 = 15
    TEST_ASSERT_EQUAL_UINT8('\0', name[15]);   // NUL terminador
}

static void test_parseSensorName_rejectsBadSensor()
{
    const char *j = "{\"node\":1,\"sensor\":4,\"nome\":\"X\"}"; // sensor>3 inválido
    uint32_t node = 0;
    uint8_t idx = 0;
    char name[16] = {0};
    TEST_ASSERT_FALSE(parseSensorName(j, strlen(j), node, idx, name, sizeof(name)).ok);
}

// ── round-trip: buildInterlocks → conteúdo verificável ───────────────────────

static void test_buildInterlocks_roundtripFields()
{
    InterlockTable tbl;
    InterlockRule r = {};
    r.id = 7;
    r.tipo = IL_SENSOR;
    r.node = 0xDEADBEEFu;
    r.sensorIdx = 3;
    r.condicao = COND_ATIVO;
    r.valorCenti = 0;
    r.histereseCenti = 0;
    r.acao = ACAO_BLOQUEAR_ABERTURA;
    r.zoneIds[0] = 1;
    r.zoneIds[1] = 2;
    r.zoneIds[2] = 3;
    r.zoneIds[3] = 4;
    r.zoneIds[4] = 0;
    r.todas = false;
    snprintf(r.mensagem, sizeof(r.mensagem), "Tamper det.");
    r.maxAbertas = 1;
    TEST_ASSERT_TRUE(tbl.upsert(r));

    char buf[1024];
    size_t n = buildInterlocks(tbl, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":7"));
    TEST_ASSERT_TRUE(contains(buf, "\"node\":3735928559")); // 0xDEADBEEF
    TEST_ASSERT_TRUE(contains(buf, "\"sensor\":3"));
    TEST_ASSERT_TRUE(contains(buf, "\"mensagem\":\"Tamper det.\""));
    TEST_ASSERT_TRUE(contains(buf, "\"zonas\":[1,2,3,4]"));
    TEST_ASSERT_TRUE(contains(buf, "\"maxAbertas\":1"));
}

// ── Fase 7b — grupos hidráulicos ─────────────────────────────────────────────

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

// ── Fase 7b — validateGroupZones ─────────────────────────────────────────────

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

// ── Fase 7b — buildGroups / groupStateLabel / buildGroupsStatus ───────────────

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

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_computeSync_states);
    RUN_TEST(test_buildOverview_json);
    RUN_TEST(test_buildOverview_truncationReturnsZero);
    RUN_TEST(test_jsonWriter_scalarArrayCommas);
    RUN_TEST(test_jsonWriter_strEscaping);
    RUN_TEST(test_buildStations_json);
    RUN_TEST(test_buildZones_json);
    RUN_TEST(test_buildPrograms_json);
    RUN_TEST(test_parseZoneUpsert_ok);
    RUN_TEST(test_parseZoneUpsert_rejectsBadRange);
    RUN_TEST(test_parseZoneDelete_ok);
    RUN_TEST(test_parseProgramUpsert_ok);
    RUN_TEST(test_parseProgramUpsert_rejectsTooManySteps);
    RUN_TEST(test_parseProgramUpsert_rejectsNoSteps);
    RUN_TEST(test_parseProgramUpsert_rejectsTruncatedStep);
    RUN_TEST(test_jsonReader_unterminatedString);
    RUN_TEST(test_jsonReader_missingValue);
    RUN_TEST(test_jsonReader_respectsLen);
    RUN_TEST(test_parseProgramToggle_ok);
    RUN_TEST(test_parseCommand_kinds);
    // Fase 6b — Task 17
    RUN_TEST(test_buildInterlocks_basic);
    RUN_TEST(test_buildInterlocks_todas_semZonas);
    RUN_TEST(test_buildInterlocks_empty);
    RUN_TEST(test_parseInterlockUpsert_ok);
    RUN_TEST(test_parseInterlockUpsert_rejectsBadId);
    RUN_TEST(test_parseInterlockUpsert_todas_comZonas);
    RUN_TEST(test_parseInterlockUpsert_zonasOverflowGuard);
    RUN_TEST(test_parseInterlockUpsert_zonasAusente);
    RUN_TEST(test_parseInterlockDelete_ok);
    RUN_TEST(test_parseInterlockDelete_rejectsBadId);
    RUN_TEST(test_buildSensorsGateway_basic);
    RUN_TEST(test_buildSensorsGateway_empty);
    RUN_TEST(test_parseMaintWindow_ok);
    RUN_TEST(test_parseMaintWindow_zero_ok);
    RUN_TEST(test_parseMaintWindow_clampMaxMinutes);
    RUN_TEST(test_parseMaintWindow_maxBoundaryAccepted);
    RUN_TEST(test_parseSensorName_ok);
    RUN_TEST(test_parseSensorName_truncatesLongName);
    RUN_TEST(test_parseSensorName_rejectsBadSensor);
    RUN_TEST(test_buildInterlocks_roundtripFields);
    // Fase 7b — grupos hidráulicos
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
    RUN_TEST(test_validateGroupZones_ok);
    RUN_TEST(test_validateGroupZones_rejectsMirrorMember);
    RUN_TEST(test_validateGroupZones_rejectsMissingMember);
    RUN_TEST(test_validateGroupZones_rejectsPumpAsMember);
    RUN_TEST(test_buildGroups_basic);
    RUN_TEST(test_buildGroups_empty);
    RUN_TEST(test_groupStateLabel_map);
    RUN_TEST(test_buildGroupsStatus_basic);
    exit(UNITY_END());
}

void loop() {}
