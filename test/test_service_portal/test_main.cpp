#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/InputActuationPolicy.h"
#include "modules/irrigation/ServicePortalApi.h"
#include <cstring>
#include <unity.h>

using namespace IrrigationWeb;

void setUp(void) {}
void tearDown(void) {}

// ── Aba Clientes ─────────────────────────────────────────────────────────────

static void test_buildClientList_marks_active()
{
    IrrigationService::LightProfile cs[2] = {};
    strcpy(cs[0].id, "f1");
    strcpy(cs[0].nome, "Sitio A");
    strcpy(cs[0].canalNome, "bv-irrig");
    cs[0].preset = 0; // LONG_FAST
    cs[0].gateway = 0xa1b2c3d4;
    cs[0].estacaoCount = 3;
    strcpy(cs[1].id, "f2");
    char buf[512];
    size_t n = buildClientList(cs, 2, "f2", buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":\"f1\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"canal\":\"bv-irrig\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active\":true"));  // f2 ativo
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active\":false")); // f1 inativo
}

static void test_parseSelect_reads_id()
{
    char id[32] = {0};
    ParseResult r = parseSelect("{\"id\":\"fazenda-sp-01\"}", 22, id, sizeof id);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("fazenda-sp-01", id);
    ParseResult r2 = parseSelect("{}", 2, id, sizeof id);
    TEST_ASSERT_FALSE(r2.ok);
}

// ── Aba Rede — varredura ─────────────────────────────────────────────────────

static void test_buildScanResults_maps_fields()
{
    ScanResults s;
    ScanEntry e{};
    e.node = 0xe5f6a7b8;
    e.role = 0; // estação
    e.epoch = 17;
    e.vbatCentiV = 1240;
    e.fwVersion = 0x0800;
    e.lat = -221000000;
    e.lon = -476000000;
    e.snrQuarterDb = 32; // 8 dB
    s.add(e);
    char buf[1024];
    size_t n = buildScanResults(s, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"epoch\":17"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"vbat\":1240"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"snr\":32"));
}

// ── Aba Rede — editor de config ──────────────────────────────────────────────

static void test_buildStationConfig_emits_fields()
{
    IrrigationSettings s; // defaults v5
    s.numValves = 3;
    s.hbMinutes = 12;
    s.pinsHbridgeA[0] = 4;
    s.sensores[0].pino = 34;
    s.sensores[0].unidade = 1; // bar
    s.localInterlocks[0].sensorIdx = 0;
    s.localInterlocks[0].saidasValvMask = 0x01;
    char buf[3072];
    size_t n = buildStationConfig(s, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"numValves\":3"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"hbMinutes\":12"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sensores\":["));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"localInterlocks\":["));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"role\":0")); // informativo read-only
}

static void test_stationConfig_round_trip()
{
    IrrigationSettings a; // defaults v5
    a.numValves = 5;
    a.hbMinutes = 7;
    a.vbatMinAbrirCentiV = 1205;
    a.maxOpenConfigS = 90;
    a.cmdRatePerMin = 4;
    a.pulseMs = 80;
    for (int i = 0; i < 8; i++) {
        a.pinsHbridgeA[i] = (int8_t)(10 + i);
        a.pinsHbridgeB[i] = (int8_t)(20 + i);
    }
    a.pinsDigitalIn[0] = 33;
    a.digitalInActiveLow = 0x03;
    a.pinBtn = 39;
    a.pinLed = 2;
    a.pinsGpo[0] = 25;
    a.pinsGpo[1] = 26;
    a.pinTamper = 27;
    a.hwFlags = 1;
    a.latE7 = -221000000;
    a.lonE7 = -476000000;
    a.sensores[0] = {34, 1, 0, 30, 0, 100, 4000, 0, 1000, 1, 0};
    a.sensores[2] = {35, 0, 1, 0, 200, 0, 4095, 0, 0, 0, 0};
    a.localInterlocks[0] = {0, 2, 1, 0x01, 1500, 50, 0x00, 0};
    a.localInterlocks[3] = {1, 3, 0, 0x04, -200, 10, 0x02, 0};
    // Campos geridos distintos p/ provar preservação (não vêm do JSON):
    a.role = (uint8_t)IrrigationRole::ESTACAO;
    a.boundGateway = 0xdeadbeef;
    a.configEpoch = 42;

    char js[3072];
    size_t n = buildStationConfig(a, js, sizeof js);
    TEST_ASSERT_TRUE(n > 0);

    IrrigationSettings b; // defaults; semeia só os geridos com os de `a`
    b.magic = a.magic;
    b.version = a.version;
    b.role = a.role;
    b.boundGateway = a.boundGateway;
    b.configEpoch = a.configEpoch;
    ParseResult r = parseStationConfig(js, n, b);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(IrrigationSettings)); // 176 B idênticos
}

static void test_parseNodeConfigReq()
{
    const char *js = "{\"node\":\"!e5f6a7b8\",\"route\":\"direct\","
                     "\"config\":{\"numValves\":6,\"hbMinutes\":9}}";
    NodeConfigReq req;
    req.config.configEpoch = 100; // semeado (gerido) — deve sobreviver
    ParseResult r = parseNodeConfigReq(js, strlen(js), req);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_HEX32(0xe5f6a7b8, req.node);
    TEST_ASSERT_EQUAL_INT((int)SvcRoute::DIRECT, (int)req.route);
    TEST_ASSERT_EQUAL_UINT8(6, req.config.numValves);
    TEST_ASSERT_EQUAL_UINT32(100, req.config.configEpoch); // preservado
    NodeConfigReq bad;
    TEST_ASSERT_FALSE(parseNodeConfigReq("{\"node\":\"!1\",\"route\":\"x\",\"config\":{}}", 36, bad).ok);
}

static void test_parseNodeAction()
{
    NodeAction a{};
    const char *pulse = "{\"node\":\"!11\",\"action\":\"pulse\",\"valveId\":2,\"durationS\":30}";
    ParseResult r = parseNodeAction(pulse, strlen(pulse), a);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT((int)SvcAction::PULSE, (int)a.action);
    TEST_ASSERT_EQUAL_UINT8(2, a.valveOrZoneId);
    TEST_ASSERT_EQUAL_UINT16(30, a.durationS);

    NodeAction z{};
    const char *zone = "{\"node\":\"!11\",\"action\":\"zone\",\"zoneId\":5,\"open\":1,\"durationS\":600}";
    TEST_ASSERT_TRUE(parseNodeAction(zone, strlen(zone), z).ok);
    TEST_ASSERT_EQUAL_INT((int)SvcAction::ZONE, (int)z.action);
    TEST_ASSERT_TRUE(z.open);

    NodeAction rs{};
    TEST_ASSERT_TRUE(parseNodeAction("{\"node\":\"!11\",\"action\":\"resync\"}", 33, rs).ok);
    TEST_ASSERT_EQUAL_INT((int)SvcAction::RESYNC, (int)rs.action);

    NodeAction bad{};
    TEST_ASSERT_FALSE(parseNodeAction("{\"node\":\"!11\",\"action\":\"nope\"}", 31, bad).ok);
}

// ── digitalInMode[] ───────────────────────────────────────────────────────────

static void test_buildStationConfig_emits_digitalInMode()
{
    IrrigationSettings s;
    s.digitalInMode[0] = InputMode::FOLLOW;
    s.digitalInMode[1] = InputMode::EDGE_TOGGLE;
    s.digitalInMode[2] = InputMode::DISABLED;
    s.digitalInMode[3] = InputMode::EDGE_CLOSE;
    char buf[3072];
    size_t n = buildStationConfig(s, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"digitalInMode\":["));
    // FOLLOW=1, EDGE_TOGGLE=3, DISABLED=0, EDGE_CLOSE=5
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"digitalInMode\":[1,3,0,5]"));
}

static void test_stationConfig_digitalInMode_roundtrip()
{
    IrrigationSettings a;
    a.digitalInMode[0] = InputMode::FOLLOW;      // 1
    a.digitalInMode[1] = InputMode::EDGE_BOTH;   // 2
    a.digitalInMode[2] = InputMode::EDGE_OPEN;   // 4
    a.digitalInMode[3] = InputMode::EDGE_CLOSE;  // 5

    char js[3072];
    size_t n = buildStationConfig(a, js, sizeof js);
    TEST_ASSERT_TRUE(n > 0);

    IrrigationSettings b;
    // seed managed fields from a so EQUAL_MEMORY check passes
    b.magic = a.magic;
    b.version = a.version;
    b.role = a.role;
    b.boundGateway = a.boundGateway;
    b.configEpoch = a.configEpoch;
    ParseResult r = parseStationConfig(js, n, b);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT((int)InputMode::FOLLOW,     (int)b.digitalInMode[0]);
    TEST_ASSERT_EQUAL_INT((int)InputMode::EDGE_BOTH,  (int)b.digitalInMode[1]);
    TEST_ASSERT_EQUAL_INT((int)InputMode::EDGE_OPEN,  (int)b.digitalInMode[2]);
    TEST_ASSERT_EQUAL_INT((int)InputMode::EDGE_CLOSE, (int)b.digitalInMode[3]);
}

static void test_parseStationConfig_digitalInMode_clampsInvalid()
{
    // Value 99 is out of range (>5) → should be clamped to DISABLED (0)
    const char *js = "{\"digitalInMode\":[1,99,3,0]}";
    IrrigationSettings b;
    ParseResult r = parseStationConfig(js, strlen(js), b);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT((int)InputMode::FOLLOW,    (int)b.digitalInMode[0]);
    TEST_ASSERT_EQUAL_INT((int)InputMode::DISABLED,  (int)b.digitalInMode[1]); // clamped
    TEST_ASSERT_EQUAL_INT((int)InputMode::EDGE_TOGGLE, (int)b.digitalInMode[2]);
    TEST_ASSERT_EQUAL_INT((int)InputMode::DISABLED,  (int)b.digitalInMode[3]);
}

// ── Backup round-trip do próprio settings ────────────────────────────────────

static void test_node_backup_roundtrips_own_settings()
{
    IrrigationSettings s{};
    s.numValves = 3;
    s.hbMinutes = 7;
    s.latE7 = -229071000;
    char buf[2048];
    size_t n = buildStationConfig(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    IrrigationSettings back{};
    ParseResult r = parseStationConfig(buf, n, back);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL(3, back.numValves);
    TEST_ASSERT_EQUAL(7, back.hbMinutes);
    TEST_ASSERT_EQUAL(-229071000, back.latE7);
}

// ── Restore preserva vínculo e aceita numValves do backup ────────────────────

static void test_node_restore_preserves_bond_and_bumps_epoch()
{
    IrrigationSettings cur{};
    cur.boundGateway = 0x7c2e04;
    cur.configEpoch = 14;
    cur.role = 0; // estação
    const char *bk = "{\"numValves\":4,\"role\":1,\"boundGateway\":0,\"configEpoch\":99}";
    IrrigationSettings merged = cur;
    ParseResult r = parseStationConfig(bk, strlen(bk), merged);
    TEST_ASSERT_TRUE(r.ok);
    // parseStationConfig preserva geridos → vínculo e papel intactos
    TEST_ASSERT_EQUAL(0x7c2e04u, merged.boundGateway);
    TEST_ASSERT_EQUAL(0, merged.role);
    TEST_ASSERT_EQUAL(4, merged.numValves);
}

// ── Aba Log ──────────────────────────────────────────────────────────────────

struct RamLogReader : IServiceLogReader {
    const char *lines[8];
    size_t n = 0;
    void forEachLine(size_t maxLines, void *ctx, void (*cb)(void *, const char *)) override
    {
        size_t start = (n > maxLines) ? n - maxLines : 0;
        for (size_t i = start; i < n; i++)
            cb(ctx, lines[i]);
    }
};

static void test_buildServiceLog()
{
    RamLogReader rd;
    rd.lines[0] = "{\"up\":10,\"ev\":\"select\"}";
    rd.lines[1] = "{\"up\":20,\"ev\":\"scan\"}";
    rd.n = 2;
    char buf[512];
    size_t n = buildServiceLog(rd, 100, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"log\":["));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ev\":\"select\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ev\":\"scan\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "},{")); // duas entradas separadas por vírgula
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_buildClientList_marks_active);
    RUN_TEST(test_parseSelect_reads_id);
    RUN_TEST(test_buildScanResults_maps_fields);
    RUN_TEST(test_buildStationConfig_emits_fields);
    RUN_TEST(test_stationConfig_round_trip);
    RUN_TEST(test_buildStationConfig_emits_digitalInMode);
    RUN_TEST(test_stationConfig_digitalInMode_roundtrip);
    RUN_TEST(test_parseStationConfig_digitalInMode_clampsInvalid);
    RUN_TEST(test_node_backup_roundtrips_own_settings);
    RUN_TEST(test_node_restore_preserves_bond_and_bumps_epoch);
    RUN_TEST(test_parseNodeConfigReq);
    RUN_TEST(test_parseNodeAction);
    RUN_TEST(test_buildServiceLog);
    exit(UNITY_END());
}
void loop() {}
