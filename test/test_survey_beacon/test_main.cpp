#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/SurveyBeacon.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_beacon_fires_first_immediately_then_at_interval()
{
    SurveyBeacon b;
    b.start(1000, 5, 60, 0, 0, false);
    TEST_ASSERT_TRUE(b.tick(1000));  // imediato
    TEST_ASSERT_FALSE(b.tick(1001)); // antes do próximo
    TEST_ASSERT_FALSE(b.tick(5999));
    TEST_ASSERT_TRUE(b.tick(6000));  // +5 s
    TEST_ASSERT_TRUE(b.tick(11000)); // +5 s
}

static void test_beacon_auto_expires_at_timeout()
{
    SurveyBeacon b;
    b.start(0, 5, 10, 0, 0, false); // timeout 10 s
    TEST_ASSERT_TRUE(b.tick(0));
    TEST_ASSERT_TRUE(b.active(9000));
    TEST_ASSERT_FALSE(b.tick(11000)); // expirou
    TEST_ASSERT_FALSE(b.active(11000));
}

static void test_beacon_stop_silences()
{
    SurveyBeacon b;
    b.start(0, 5, 60, 0, 0, false);
    b.stop();
    TEST_ASSERT_FALSE(b.tick(0));
    TEST_ASSERT_FALSE(b.active(0));
}

static void test_beacon_coord_getters()
{
    SurveyBeacon b;
    b.start(0, 5, 60, -221000000, -476000000, true);
    TEST_ASSERT_TRUE(b.hasCoord());
    TEST_ASSERT_EQUAL_INT32(-221000000, b.latE7());
    TEST_ASSERT_EQUAL_INT32(-476000000, b.lonE7());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_beacon_fires_first_immediately_then_at_interval);
    RUN_TEST(test_beacon_auto_expires_at_timeout);
    RUN_TEST(test_beacon_stop_silences);
    RUN_TEST(test_beacon_coord_getters);
    exit(UNITY_END());
}
void loop() {}
