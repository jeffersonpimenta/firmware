#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ProgramScheduler.h"
#include <unity.h>

using T = SchedAction::Type;

void setUp(void) {}
void tearDown(void) {}

// Segunda-feira 1970-01-05 00:00 UTC = epoch 345600. bit segunda = 1<<1.
static constexpr uint32_t MONDAY = 345600;

static Program mkProg(uint8_t id, uint16_t startMin, uint8_t nSteps)
{
    Program p;
    p.id = id;
    p.daysMask = 1 << 1; // segunda
    p.startMinute = startMin;
    p.stepCount = nSteps;
    for (uint8_t i = 0; i < nSteps; i++) {
        p.steps[i].zoneId = 10 + i;
        p.steps[i].durationMin = 2;
    }
    return p;
}

static SchedAction drain(ProgramScheduler &s, uint32_t t, SchedAction *extra = nullptr)
{
    SchedAction a = s.tick(t);
    if (extra)
        *extra = s.tick(t);
    else
        s.tick(t);
    return a;
}

static void test_firesOnDayAndMinute_runsSequence()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 2)); // segunda 08:00, 2 zonas de 2 min

    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 479 * 60).type); // 07:59
    SchedAction a = s.tick(MONDAY + 480 * 60);             // 08:00
    TEST_ASSERT_EQUAL(T::OPEN, a.type);
    TEST_ASSERT_EQUAL_UINT8(10, a.zoneId);
    TEST_ASSERT_EQUAL_UINT16(120, a.durationS);
    TEST_ASSERT_TRUE(s.running());

    // 2 min depois: fecha z10, abre z11 (dois ticks)
    SchedAction c = s.tick(MONDAY + 482 * 60);
    TEST_ASSERT_EQUAL(T::CLOSE, c.type);
    TEST_ASSERT_EQUAL_UINT8(10, c.zoneId);
    SchedAction o = s.tick(MONDAY + 482 * 60);
    TEST_ASSERT_EQUAL(T::OPEN, o.type);
    TEST_ASSERT_EQUAL_UINT8(11, o.zoneId);

    // +2 min: fecha z11 e termina
    SchedAction f = s.tick(MONDAY + 484 * 60);
    TEST_ASSERT_EQUAL(T::CLOSE, f.type);
    TEST_ASSERT_EQUAL_UINT8(11, f.zoneId);
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 484 * 60).type);
    TEST_ASSERT_FALSE(s.running());
}

static void test_firesOncePerMinute_notOnWrongDay()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 1));
    TEST_ASSERT_EQUAL(T::OPEN, s.tick(MONDAY + 480 * 60).type);
    s.abort();
    // mesmo minuto de novo: não redispara
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 480 * 60 + 30).type);
    // terça, mesmo horário: fora da máscara
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 86400 + 480 * 60).type);
}

static void test_secondProgramIgnoredWhileRunning()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 1));
    Program p2 = mkProg(2, 481, 1);
    p2.steps[0].zoneId = 99;
    s.upsert(p2);
    TEST_ASSERT_EQUAL(T::OPEN, s.tick(MONDAY + 480 * 60).type);
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 481 * 60).type); // p2 ignorado
    TEST_ASSERT_TRUE(s.running());
}

static void test_disabledAndZeroTimeIgnored()
{
    ProgramScheduler s;
    Program p = mkProg(1, 480, 1);
    p.enabled = false;
    s.upsert(p);
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 480 * 60).type);
    TEST_ASSERT_EQUAL(T::NONE, s.tick(0).type); // sem RTC válido: nada dispara
}

static void test_abortStopsRun()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 2));
    s.tick(MONDAY + 480 * 60);
    TEST_ASSERT_EQUAL_UINT8(10, s.currentZone());
    s.abort();
    TEST_ASSERT_FALSE(s.running());
    TEST_ASSERT_EQUAL(T::NONE, s.tick(MONDAY + 482 * 60).type);
}

static void test_serializeRoundTrip()
{
    ProgramScheduler s;
    s.upsert(mkProg(1, 480, 2));
    s.upsert(mkProg(3, 1200, 1));
    uint8_t buf[512];
    size_t n = s.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    ProgramScheduler c;
    TEST_ASSERT_TRUE(c.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(2, c.count());
    TEST_ASSERT_EQUAL(T::OPEN, c.tick(MONDAY + 480 * 60).type);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_firesOnDayAndMinute_runsSequence);
    RUN_TEST(test_firesOncePerMinute_notOnWrongDay);
    RUN_TEST(test_secondProgramIgnoredWhileRunning);
    RUN_TEST(test_disabledAndZeroTimeIgnored);
    RUN_TEST(test_abortStopsRun);
    RUN_TEST(test_serializeRoundTrip);
    exit(UNITY_END());
}

void loop() {}
