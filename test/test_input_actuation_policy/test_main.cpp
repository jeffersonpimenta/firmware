#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/InputActuationPolicy.h"
#include <unity.h>

using Intent = InputActuationPolicy::Intent;

void setUp(void) {}
void tearDown(void) {}

// estabiliza um nível por >= debounceMs chamando a cada 25 ms; retorna a última intenção != NONE
static Intent settle(InputActuationPolicy &p, bool rawHigh, uint32_t from, uint32_t spanMs = 125)
{
    Intent last = Intent::NONE;
    for (uint32_t t = from; t <= from + spanMs; t += 25) {
        Intent a = p.update(rawHigh, t);
        if (a != Intent::NONE)
            last = a;
    }
    return last;
}

static void test_disabled_never_fires()
{
    InputActuationPolicy p;
    p.configure(InputMode::DISABLED, false);
    TEST_ASSERT_EQUAL(Intent::NONE, settle(p, true, 1000));
    TEST_ASSERT_FALSE(p.active());
}

static void test_follow_rise_open_fall_close()
{
    InputActuationPolicy p;
    p.configure(InputMode::FOLLOW, false);
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(p, true, 1000));
    TEST_ASSERT_TRUE(p.active());
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(p, false, 5000));
    TEST_ASSERT_FALSE(p.active());
}

static void test_follow_renews_every_60s()
{
    InputActuationPolicy p;
    p.configure(InputMode::FOLLOW, false);
    settle(p, true, 1000);
    TEST_ASSERT_EQUAL(Intent::NONE, p.update(true, 30000));
    TEST_ASSERT_EQUAL(Intent::OPEN, p.update(true, 1000 + 125 + 60000));
}

static void test_follow_active_low_inverts()
{
    InputActuationPolicy p;
    p.configure(InputMode::FOLLOW, true); // ativo-baixo: LOW = ativo
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(p, false, 1000));
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(p, true, 5000));
}

static void test_edge_both_rise_open_fall_close_no_renew()
{
    InputActuationPolicy p;
    p.configure(InputMode::EDGE_BOTH, false);
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(p, true, 1000, 75));
    TEST_ASSERT_EQUAL(Intent::NONE, p.update(true, 1000 + 75 + 60000)); // sem renovação
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(p, false, 200000, 75));
}

static void test_edge_toggle_once_per_press()
{
    InputActuationPolicy p;
    p.configure(InputMode::EDGE_TOGGLE, false);
    TEST_ASSERT_EQUAL(Intent::TOGGLE, settle(p, true, 1000, 75));
    TEST_ASSERT_EQUAL(Intent::NONE, settle(p, true, 2000, 75)); // segurar não re-dispara
    settle(p, false, 3000, 75);                                 // solta (rearma)
    TEST_ASSERT_EQUAL(Intent::TOGGLE, settle(p, true, 4000, 75));
}

static void test_edge_open_and_close_fixed()
{
    InputActuationPolicy po;
    po.configure(InputMode::EDGE_OPEN, false);
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(po, true, 1000, 75));
    InputActuationPolicy pc;
    pc.configure(InputMode::EDGE_CLOSE, false);
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(pc, true, 1000, 75));
}

// EDGE_OPEN / EDGE_CLOSE: holding high does NOT re-fire; releasing then pressing again DOES.
static void test_edge_open_rearm_after_release()
{
    InputActuationPolicy p;
    p.configure(InputMode::EDGE_OPEN, false);

    // (a) First press fires OPEN
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(p, true, 1000, 75));

    // (b) Holding high (continued updates past debounce) must NOT fire again
    TEST_ASSERT_EQUAL(Intent::NONE, settle(p, true, 2000, 75));
    TEST_ASSERT_EQUAL(Intent::NONE, p.update(true, 3000));

    // (c) Release (settle low) — no intent from falling edge in EDGE_OPEN
    TEST_ASSERT_EQUAL(Intent::NONE, settle(p, false, 4000, 75));

    // (d) Press again DOES fire OPEN
    TEST_ASSERT_EQUAL(Intent::OPEN, settle(p, true, 5000, 75));
}

static void test_edge_close_rearm_after_release()
{
    InputActuationPolicy p;
    p.configure(InputMode::EDGE_CLOSE, false);

    // (a) First press fires CLOSE
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(p, true, 1000, 75));

    // (b) Holding high does NOT fire again
    TEST_ASSERT_EQUAL(Intent::NONE, settle(p, true, 2000, 75));
    TEST_ASSERT_EQUAL(Intent::NONE, p.update(true, 3000));

    // (c) Release — no intent from falling edge in EDGE_CLOSE
    TEST_ASSERT_EQUAL(Intent::NONE, settle(p, false, 4000, 75));

    // (d) Press again DOES fire CLOSE
    TEST_ASSERT_EQUAL(Intent::CLOSE, settle(p, true, 5000, 75));
}

static void test_glitch_shorter_than_debounce_ignored()
{
    InputActuationPolicy p;
    p.configure(InputMode::FOLLOW, false);
    p.update(true, 1000);
    p.update(false, 1050); // caiu antes de 100 ms
    TEST_ASSERT_EQUAL(Intent::NONE, p.update(false, 1200));
    TEST_ASSERT_FALSE(p.active());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_disabled_never_fires);
    RUN_TEST(test_follow_rise_open_fall_close);
    RUN_TEST(test_follow_renews_every_60s);
    RUN_TEST(test_follow_active_low_inverts);
    RUN_TEST(test_edge_both_rise_open_fall_close_no_renew);
    RUN_TEST(test_edge_toggle_once_per_press);
    RUN_TEST(test_edge_open_and_close_fixed);
    RUN_TEST(test_edge_open_rearm_after_release);
    RUN_TEST(test_edge_close_rearm_after_release);
    RUN_TEST(test_glitch_shorter_than_debounce_ignored);
    exit(UNITY_END());
}
void loop() {}
