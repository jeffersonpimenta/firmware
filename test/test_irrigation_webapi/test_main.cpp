#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/GatewayTables.h"
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
    exit(UNITY_END());
}

void loop() {}
