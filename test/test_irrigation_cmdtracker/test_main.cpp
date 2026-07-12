#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/CommandTracker.h"
#include <unity.h>

using What = CommandTracker::Retry::What;

void setUp(void) {}
void tearDown(void) {}

static void test_ackClearsPending()
{
    CommandTracker t;
    TEST_ASSERT_TRUE(t.track(100, 0x11, 1, 1, 600, 3, 1000));
    TEST_ASSERT_TRUE(t.onAck(0x11, 100));
    TEST_ASSERT_FALSE(t.onAck(0x11, 100)); // já resolvido
    TEST_ASSERT_EQUAL(What::NONE, t.poll(20000).what);
}

static void test_timeoutYieldsResendThenFailed()
{
    CommandTracker t;
    t.track(100, 0x11, 1, 1, 600, 2, 1000);
    TEST_ASSERT_EQUAL(What::NONE, t.poll(8999).what); // 7.999 s
    auto r = t.poll(9000);                            // 8 s
    TEST_ASSERT_EQUAL(What::RESEND, r.what);
    TEST_ASSERT_EQUAL_UINT8(1, r.attemptsLeft);
    TEST_ASSERT_EQUAL_HEX32(0x11, r.node);

    t.retrack(101, r, 9000);
    auto f = t.poll(17000); // +8 s
    TEST_ASSERT_EQUAL(What::FAILED, f.what);
    TEST_ASSERT_EQUAL_UINT8(1, f.zoneId);
    TEST_ASSERT_EQUAL(What::NONE, t.poll(30000).what);
}

static void test_wrongNodeOrSeqDoesNotClear()
{
    CommandTracker t;
    t.track(100, 0x11, 1, 1, 600, 3, 1000);
    TEST_ASSERT_FALSE(t.onAck(0x22, 100));
    TEST_ASSERT_FALSE(t.onAck(0x11, 999));
    TEST_ASSERT_EQUAL(What::RESEND, t.poll(9001).what);
}

static void test_queueFullRejects()
{
    CommandTracker t;
    for (uint32_t i = 0; i < CommandTracker::MAX_PENDING; i++)
        TEST_ASSERT_TRUE(t.track(100 + i, i + 1, 1, 1, 60, 3, 0));
    TEST_ASSERT_FALSE(t.track(200, 99, 1, 1, 60, 3, 0));
    TEST_ASSERT_TRUE(t.onAck(1, 100));
    TEST_ASSERT_TRUE(t.track(200, 99, 1, 1, 60, 3, 0));
}

static void test_rolloverSafe()
{
    CommandTracker t;
    t.track(100, 0x11, 1, 1, 60, 2, 0xFFFFF000u);
    TEST_ASSERT_EQUAL(What::NONE, t.poll(0xFFFFF000u + 7000).what);
    TEST_ASSERT_EQUAL(What::RESEND, t.poll(0xFFFFF000u + 8000).what); // atravessa o wrap
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_ackClearsPending);
    RUN_TEST(test_timeoutYieldsResendThenFailed);
    RUN_TEST(test_wrongNodeOrSeqDoesNotClear);
    RUN_TEST(test_queueFullRejects);
    RUN_TEST(test_rolloverSafe);
    exit(UNITY_END());
}

void loop() {}
