#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/RateLimiter.h"
#include "modules/irrigation/SeqTable.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_firstSeqFromSender_accepted()
{
    SeqTable t;
    TEST_ASSERT_TRUE(t.checkAndUpdate(0xa1b2c3d4, 100));
    TEST_ASSERT_EQUAL_UINT32(100, t.lastSeq(0xa1b2c3d4));
}

static void test_replayAndOldSeq_rejected()
{
    SeqTable t;
    t.checkAndUpdate(0xa1b2c3d4, 100);
    TEST_ASSERT_FALSE(t.checkAndUpdate(0xa1b2c3d4, 100)); // replay
    TEST_ASSERT_FALSE(t.checkAndUpdate(0xa1b2c3d4, 99));  // antiga
    TEST_ASSERT_TRUE(t.checkAndUpdate(0xa1b2c3d4, 101));
}

static void test_sendersIndependent()
{
    SeqTable t;
    TEST_ASSERT_TRUE(t.checkAndUpdate(0x11111111, 50));
    TEST_ASSERT_TRUE(t.checkAndUpdate(0x22222222, 50)); // mesmo seq, remetente diferente
}

static void test_tableFull_overwritesLowestSeq()
{
    SeqTable t;
    for (uint32_t i = 0; i < SeqTable::MAX_PEERS; i++)
        TEST_ASSERT_TRUE(t.checkAndUpdate(0x1000 + i, 10 + i));
    // Tabela cheia: novo remetente desaloja a entrada de menor seq (0x1000)
    TEST_ASSERT_TRUE(t.checkAndUpdate(0x9999, 5));
    TEST_ASSERT_EQUAL_UINT32(5, t.lastSeq(0x9999));
    TEST_ASSERT_EQUAL_UINT32(0, t.lastSeq(0x1000)); // esquecido
}

static void test_rateLimiter_blocksAboveLimit()
{
    RateLimiter rl(3);
    TEST_ASSERT_TRUE(rl.allow(1000));
    TEST_ASSERT_TRUE(rl.allow(2000));
    TEST_ASSERT_TRUE(rl.allow(3000));
    TEST_ASSERT_FALSE(rl.allow(4000));
}

static void test_rateLimiter_windowResets()
{
    RateLimiter rl(2);
    TEST_ASSERT_TRUE(rl.allow(1000));
    TEST_ASSERT_TRUE(rl.allow(2000));
    TEST_ASSERT_FALSE(rl.allow(3000));
    TEST_ASSERT_TRUE(rl.allow(1000 + 60000)); // nova janela de 60 s
}

static void test_rateLimiter_millisRollover()
{
    RateLimiter rl(1);
    TEST_ASSERT_TRUE(rl.allow(0xFFFFF000u));
    TEST_ASSERT_FALSE(rl.allow(0xFFFFFF00u));
    TEST_ASSERT_TRUE(rl.allow(0xFFFFF000u + 60000)); // atravessa o wrap
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_firstSeqFromSender_accepted);
    RUN_TEST(test_replayAndOldSeq_rejected);
    RUN_TEST(test_sendersIndependent);
    RUN_TEST(test_tableFull_overwritesLowestSeq);
    RUN_TEST(test_rateLimiter_blocksAboveLimit);
    RUN_TEST(test_rateLimiter_windowResets);
    RUN_TEST(test_rateLimiter_millisRollover);
    exit(UNITY_END());
}

void loop() {}
