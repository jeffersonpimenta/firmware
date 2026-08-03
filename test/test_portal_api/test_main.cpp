#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/AuditLog.h"
#include "modules/irrigation/PortalApi.h"
#include <stdio.h>
#include <string.h>
#include <unity.h>

using namespace IrrigationWeb;

void setUp(void) {}
void tearDown(void) {}

static bool contains(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }

static void test_buildNodeState_json()
{
    NodeStateCtx c = {};
    c.role = 0; // ESTACAO
    c.name = "Pasto Norte";
    c.boundGateway = 0x1234;
    c.configEpoch = 7;
    c.safeMode = false;
    c.numValves = 2;
    c.numGpos = 1;
    c.valveStates = 0x01;
    c.gpoStates = 0;
    c.vbatCentiV = 1250;
    c.flags = 0;
    c.apSecondsLeft = 540;
    char buf[512];
    size_t n = buildNodeState(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"role\":0"));
    TEST_ASSERT_TRUE(contains(buf, "\"name\":\"Pasto Norte\""));
    TEST_ASSERT_TRUE(contains(buf, "\"boundGateway\":4660")); // 0x1234
    TEST_ASSERT_TRUE(contains(buf, "\"numGpos\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"valveStates\":1"));
    TEST_ASSERT_TRUE(contains(buf, "\"vbatCentiV\":1250"));
    TEST_ASSERT_TRUE(contains(buf, "\"apSecondsLeft\":540"));
}

static void test_buildNodeState_truncationReturnsZero()
{
    NodeStateCtx c = {};
    c.name = "x";
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(0, buildNodeState(c, buf, sizeof(buf)));
}

static void test_parsePulse_valid()
{
    PortalPulseReq p = {};
    ParseResult r = parsePulse("{\"valveId\":1,\"durationS\":10}", 28, p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(1, p.valveId);
    TEST_ASSERT_EQUAL_UINT16(10, p.durationS);
}

static void test_parsePulse_rejectsBadValve()
{
    PortalPulseReq p = {};
    ParseResult r = parsePulse("{\"valveId\":9,\"durationS\":10}", 28, p);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_parsePulse_rejectsBadDuration()
{
    PortalPulseReq p = {};
    ParseResult r = parsePulse("{\"valveId\":0,\"durationS\":0}", 27, p);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_parseNetCommand_open()
{
    NetCommand c = {};
    ParseResult r = parseNetCommand("{\"kind\":\"open\",\"zoneId\":3,\"durationS\":300}", 41, c);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(3, c.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, c.action);
    TEST_ASSERT_EQUAL_UINT16(300, c.durationS);
}

static void test_parseNetCommand_close()
{
    NetCommand c = {};
    ParseResult r = parseNetCommand("{\"kind\":\"close\",\"zoneId\":3}", 27, c);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(0, c.action);
}

static void test_parseNetCommand_rejectsBadZone()
{
    NetCommand c = {};
    ParseResult r = parseNetCommand("{\"kind\":\"open\",\"zoneId\":0,\"durationS\":10}", 40, c);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_buildRoster_gateway()
{
    ZoneTable zt;
    Zone z = {};
    z.id = 3;
    snprintf(z.name, sizeof(z.name), "Horta");
    z.node = 0xAA;
    zt.upsert(z);
    char buf[512];
    size_t n = buildRoster(&zt, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":3"));
    TEST_ASSERT_TRUE(contains(buf, "\"name\":\"Horta\""));
}

static void test_buildRoster_stationEmpty()
{
    char buf[64];
    size_t n = buildRoster(nullptr, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("[]", buf);
}

static void test_buildSensors_json()
{
    PortalSensorsCtx c = {};
    c.count = 2;
    c.items[0] = {0, 1, 1, 152}; // analógico, bar, 1,52
    c.items[1] = {1, 0, 0, 100}; // digital ativo
    char buf[256];
    size_t n = buildSensors(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"id\":0"));
    TEST_ASSERT_TRUE(contains(buf, "\"unidade\":\"bar\""));
    TEST_ASSERT_TRUE(contains(buf, "\"valor\":152"));
    TEST_ASSERT_TRUE(contains(buf, "\"tipo\":0"));
}

static void test_buildPortalLog_json()
{
    AuditRecord store[8];
    AuditLog log(store, 8);
    AuditRecord r1 = {};
    r1.tsSecs = 10;
    r1.origin = 4;
    r1.action = 0;
    r1.target = 1;
    r1.result = 0;
    AuditRecord r2 = {};
    r2.tsSecs = 20;
    r2.origin = 3;
    r2.action = 1;
    r2.target = 2;
    r2.result = 1;
    log.append(r1);
    log.append(r2);
    char buf[512];
    size_t n = buildPortalLog(log, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"ts\":20"));
    TEST_ASSERT_TRUE(contains(buf, "\"origem\":3"));
    // mais recente primeiro: ts=20 aparece ANTES de ts=10 no JSON
    const char *p20 = strstr(buf, "\"ts\":20");
    const char *p10 = strstr(buf, "\"ts\":10");
    TEST_ASSERT_NOT_NULL(p20);
    TEST_ASSERT_NOT_NULL(p10);
    TEST_ASSERT_TRUE(p20 < p10);
}

static void test_parseGpoReq_valid()
{
    PortalGpoReq g = {};
    const char *j = "{\"gpo\":1,\"action\":1,\"durationS\":0,\"confirm\":true}";
    ParseResult r = parseGpoReq(j, strlen(j), g);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(1, g.gpoId);
    TEST_ASSERT_EQUAL_UINT8(1, g.action);
    TEST_ASSERT_EQUAL_UINT16(0, g.durationS);
    TEST_ASSERT_TRUE(g.confirm);
}

static void test_parseGpoReq_confirmAbsentDefaultsFalse()
{
    PortalGpoReq g = {};
    const char *j = "{\"gpo\":0,\"action\":1,\"durationS\":60}";
    ParseResult r = parseGpoReq(j, strlen(j), g);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(g.confirm);
}

static void test_parseGpoReq_rejectsBadGpo()
{
    PortalGpoReq g = {};
    const char *j = "{\"gpo\":5,\"action\":1,\"durationS\":0}";
    ParseResult r = parseGpoReq(j, strlen(j), g);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_coords_roundtrip()
{
    PortalCoords c = {};
    const char *j = "{\"latE7\":-221234560,\"lonE7\":-476543210}";
    ParseResult r = parseCoords(j, strlen(j), c);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT32(-221234560, c.latE7);
    TEST_ASSERT_EQUAL_INT32(-476543210, c.lonE7);
    char buf[128];
    size_t n = buildCoords(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"latE7\":-221234560"));
}

static void test_parseCoords_rejectsOutOfRange()
{
    PortalCoords c = {};
    const char *j = "{\"latE7\":2000000000,\"lonE7\":0}";
    ParseResult r = parseCoords(j, strlen(j), c);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_parseProvision_valid()
{
    ProvisionReq p = {};
    const char *j = "{\"role\":1,\"farmName\":\"Bela Vista\"}";
    ParseResult r = parseProvision(j, strlen(j), p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(1, p.role);
    TEST_ASSERT_TRUE(p.hasFarmName);
    TEST_ASSERT_EQUAL_STRING("Bela Vista", p.farmName);
}

static void test_parseProvision_rejectsBadRole()
{
    ProvisionReq p = {};
    const char *j = "{\"role\":9}";
    ParseResult r = parseProvision(j, strlen(j), p);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_parseProvision_farmNameOptional()
{
    ProvisionReq p = {};
    const char *j = "{\"role\":0}";
    ParseResult r = parseProvision(j, strlen(j), p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(0, p.role);
    TEST_ASSERT_FALSE(p.hasFarmName);
}

static void test_buildNodeState_provisioned()
{
    NodeStateCtx c = {};
    c.name = "x";
    c.provisioned = true;
    char buf[512];
    size_t n = buildNodeState(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"provisioned\":true"));
}

static void test_buildNodeState_uptimeS()
{
    NodeStateCtx c = {};
    c.role = 2;
    c.name = "";
    c.uptimeS = 1234567;
    char buf[512];
    size_t n = buildNodeState(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"uptimeS\":1234567"));
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_buildNodeState_json);
    RUN_TEST(test_buildNodeState_truncationReturnsZero);
    RUN_TEST(test_parsePulse_valid);
    RUN_TEST(test_parsePulse_rejectsBadValve);
    RUN_TEST(test_parsePulse_rejectsBadDuration);
    RUN_TEST(test_parseNetCommand_open);
    RUN_TEST(test_parseNetCommand_close);
    RUN_TEST(test_parseNetCommand_rejectsBadZone);
    RUN_TEST(test_buildRoster_gateway);
    RUN_TEST(test_buildRoster_stationEmpty);
    RUN_TEST(test_buildSensors_json);
    RUN_TEST(test_buildPortalLog_json);
    RUN_TEST(test_parseGpoReq_valid);
    RUN_TEST(test_parseGpoReq_confirmAbsentDefaultsFalse);
    RUN_TEST(test_parseGpoReq_rejectsBadGpo);
    RUN_TEST(test_coords_roundtrip);
    RUN_TEST(test_parseCoords_rejectsOutOfRange);
    RUN_TEST(test_parseProvision_valid);
    RUN_TEST(test_parseProvision_rejectsBadRole);
    RUN_TEST(test_parseProvision_farmNameOptional);
    RUN_TEST(test_buildNodeState_provisioned);
    RUN_TEST(test_buildNodeState_uptimeS);
    UNITY_END();
}

void loop() {}
