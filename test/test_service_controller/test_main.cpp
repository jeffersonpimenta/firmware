#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ServiceBackup.h"
#include "modules/irrigation/ServiceController.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_channelFromProfile_decodes()
{
    IrrigationService::LightProfile p{};
    strcpy(p.canalNome, "bv-irrig");
    strcpy(p.pskB64, "1PG7Og==");
    p.preset = 1;
    RetunePlan r = channelFromProfile(p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("bv-irrig", r.name);
    TEST_ASSERT_EQUAL_size_t(4, r.pskLen);
    TEST_ASSERT_EQUAL_UINT8(1, r.preset);
}

static void test_channelFromProfile_clamps_long_name()
{
    IrrigationService::LightProfile p{};
    strcpy(p.canalNome, "abc");
    strcpy(p.pskB64, "1PG7Og==");
    RetunePlan r = channelFromProfile(p);
    TEST_ASSERT_TRUE(strlen(r.name) <= 12);
}

static void test_channelFromProfile_bad_psk_not_ok()
{
    IrrigationService::LightProfile p{};
    strcpy(p.canalNome, "c");
    strcpy(p.pskB64, "!!!!");
    RetunePlan r = channelFromProfile(p);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_route_via_gateway_when_reachable()
{
    RouteDecision d = decideConfigRoute(true, 17);
    TEST_ASSERT_EQUAL_INT((int)ConfigRoute::VIA_GATEWAY, (int)d.route);
    TEST_ASSERT_EQUAL_UINT32(0, d.epochToWrite);
}

static void test_route_direct_bumps_epoch()
{
    RouteDecision d = decideConfigRoute(false, 17);
    TEST_ASSERT_EQUAL_INT((int)ConfigRoute::DIRECT, (int)d.route);
    TEST_ASSERT_EQUAL_UINT32(18, d.epochToWrite);
}

static void test_resync_helpers()
{
    TEST_ASSERT_TRUE(needsResync(false));
    TEST_ASSERT_FALSE(needsResync(true));
    TEST_ASSERT_EQUAL_UINT32(4214, resumeSeqFrom(4213));
}

static void test_scan_dedupes_by_node()
{
    ScanResults s;
    ScanEntry a{};
    a.node = 0x11;
    a.epoch = 1;
    s.add(a);
    a.epoch = 5; // same node, newer read
    s.add(a);
    ScanEntry b{};
    b.node = 0x22;
    s.add(b);
    TEST_ASSERT_EQUAL_size_t(2, s.count());
    TEST_ASSERT_EQUAL_UINT32(5, s.at(0)->epoch);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_channelFromProfile_decodes);
    RUN_TEST(test_channelFromProfile_clamps_long_name);
    RUN_TEST(test_channelFromProfile_bad_psk_not_ok);
    RUN_TEST(test_route_via_gateway_when_reachable);
    RUN_TEST(test_route_direct_bumps_epoch);
    RUN_TEST(test_resync_helpers);
    RUN_TEST(test_scan_dedupes_by_node);
    exit(UNITY_END());
}
void loop() {}
