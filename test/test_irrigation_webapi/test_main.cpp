#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationWebApi.h"
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

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_computeSync_states);
    RUN_TEST(test_buildOverview_json);
    RUN_TEST(test_buildOverview_truncationReturnsZero);
    exit(UNITY_END());
}

void loop() {}
