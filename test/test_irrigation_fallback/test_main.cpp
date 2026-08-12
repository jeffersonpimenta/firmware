#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/RemoteFallbackTracker.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_arm_then_clear_before_timeout_no_fire()
{
    RemoteFallbackTracker t(2000);
    t.arm(1, 1000);
    t.clear(1); // REMOTE_LED chegou
    uint8_t out[4];
    TEST_ASSERT_EQUAL_UINT(0, t.takeExpired(4000, out, 4));
}

static void test_arm_expires_fires_once()
{
    RemoteFallbackTracker t(2000);
    t.arm(2, 1000);
    uint8_t out[4];
    TEST_ASSERT_EQUAL_UINT(0, t.takeExpired(2500, out, 4)); // 1000+2000=3000, ainda não
    TEST_ASSERT_EQUAL_UINT(1, t.takeExpired(3001, out, 4));
    TEST_ASSERT_EQUAL_UINT8(2, out[0]);
    TEST_ASSERT_EQUAL_UINT(0, t.takeExpired(9000, out, 4)); // não redispara
}

static void test_rearm_same_index_resets_deadline()
{
    RemoteFallbackTracker t(2000);
    t.arm(0, 1000);
    t.arm(0, 5000); // repressionou antes de expirar → novo prazo
    uint8_t out[4];
    TEST_ASSERT_EQUAL_UINT(0, t.takeExpired(6000, out, 4));
    TEST_ASSERT_EQUAL_UINT(1, t.takeExpired(7001, out, 4));
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_arm_then_clear_before_timeout_no_fire);
    RUN_TEST(test_arm_expires_fires_once);
    RUN_TEST(test_rearm_same_index_resets_deadline);
    exit(UNITY_END());
}

void loop() {}
