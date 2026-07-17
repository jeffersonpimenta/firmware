#include "Arduino.h"
#include "TestUtil.h"
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
    UNITY_END();
}

void loop() {}
