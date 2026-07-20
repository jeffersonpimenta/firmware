#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/GpoController.h"
#include <unity.h>

struct FakeGpoDriver : IGpoDriver {
    bool level[GpoController::MAX_GPO] = {};
    int setCount = 0;
    void set(uint8_t i, bool on) override { level[i] = on; setCount++; }
};

void setUp(void) {}
void tearDown(void) {}

static void test_bistable_stays_on()
{
    FakeGpoDriver d;
    GpoController g(d, 2);
    TEST_ASSERT_EQUAL(GpoController::Result::OK, g.command(0, 1, 0, 1000));
    TEST_ASSERT_TRUE(d.level[0]);
    g.tick(1000 + 24u * 3600u * 1000u); // 24 h depois: continua ligado
    TEST_ASSERT_TRUE(g.isOn(0));
    TEST_ASSERT_EQUAL_UINT8(0x1, g.states());
    g.command(0, 0, 0, 0);
    TEST_ASSERT_FALSE(d.level[0]);
}

static void test_timed_auto_off()
{
    FakeGpoDriver d;
    GpoController g(d, 2);
    g.command(1, 1, 60, 0); // 60 s
    g.tick(59999);
    TEST_ASSERT_TRUE(g.isOn(1));
    g.tick(60000);
    TEST_ASSERT_FALSE(g.isOn(1)); // fail-safe local
    TEST_ASSERT_FALSE(d.level[1]);
}

static void test_invalid_id_and_alloff()
{
    FakeGpoDriver d;
    GpoController g(d, 1);
    TEST_ASSERT_EQUAL(GpoController::Result::INVALID_ID, g.command(1, 1, 0, 0));
    g.command(0, 1, 0, 0);
    g.allOff();
    TEST_ASSERT_EQUAL_UINT8(0, g.states());
}

static void test_shrink_turns_off_removed()
{
    FakeGpoDriver d;
    GpoController g(d, 2);
    g.command(1, 1, 0, 0);
    g.setNumGpos(1);
    TEST_ASSERT_FALSE(d.level[1]);
    TEST_ASSERT_EQUAL_UINT8(0, g.states());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_bistable_stays_on);
    RUN_TEST(test_timed_auto_off);
    RUN_TEST(test_invalid_id_and_alloff);
    RUN_TEST(test_shrink_turns_off_removed);
    exit(UNITY_END());
}

void loop() {}
