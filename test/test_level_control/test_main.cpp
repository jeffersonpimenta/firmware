#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/LevelControlEngine.h"
#include "modules/irrigation/LevelControlTable.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

using Act = LevelIntent::Act;

static LevelRule rule1()
{
    LevelRule r{};
    r.id = 1;
    r.sensorNode = 0xaa;
    r.sensorIdx = 0;
    r.ligaQuandoAtivo = true; // ATIVO = nível baixo = liga
    r.targetZoneId = 5;
    r.minOnS = 30;
    r.minOffS = 30;
    r.staleTimeoutS = 90;
    return r;
}

// Helper: 1 regra, 1 input, devolve o único intent (ou NONE).
static LevelIntent step(LevelControlEngine &e, LevelControlTable &t, LevelInput in, uint32_t nowMs)
{
    LevelIntent out[LevelControlTable::MAX];
    size_t n = e.evaluate(t, &in, 1, nowMs, out, LevelControlTable::MAX);
    if (n == 0) {
        LevelIntent none{};
        return none;
    }
    return out[0];
}

static LevelInput freshInput(bool active) { return LevelInput{true, active, true}; }

static void test_starts_when_low_after_minOff()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    // t=0: nível baixo (ativo), minOff=30s não cumprido desde lastOff(0)? lastOff=0, now=1000ms<30000 → não liga
    TEST_ASSERT_EQUAL(Act::NONE, step(e, t, freshInput(true), 1000).act);
    // t=31s: minOff cumprido → START
    LevelIntent i = step(e, t, freshInput(true), 31000);
    TEST_ASSERT_EQUAL(Act::START, i.act);
    TEST_ASSERT_EQUAL_UINT8(5, i.zoneId);
    TEST_ASSERT_EQUAL_UINT16(LevelControlEngine::OPEN_CEILING_S, i.durS);
}

static void test_stops_when_full_after_minOn()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    step(e, t, freshInput(true), 31000);              // START @31s
    // nível encheu (inativo) @40s: minOn=30s desde lastOn(31s) não cumprido → segura (RENEW não, <60s)
    TEST_ASSERT_EQUAL(Act::NONE, step(e, t, freshInput(false), 40000).act);
    // @62s: minOn cumprido → STOP
    TEST_ASSERT_EQUAL(Act::STOP, step(e, t, freshInput(false), 62000).act);
}

static void test_renew_while_on()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    step(e, t, freshInput(true), 31000);              // START @31s, lastRenew=31s
    // @95s (>=31s+60s), ainda quer encher → RENEW
    LevelIntent i = step(e, t, freshInput(true), 95000);
    TEST_ASSERT_EQUAL(Act::RENEW, i.act);
    TEST_ASSERT_EQUAL_UINT16(LevelControlEngine::OPEN_CEILING_S, i.durS);
}

static void test_stale_stops_and_alerts_once()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    step(e, t, freshInput(true), 31000);              // START @31s (ligada)
    // boia sumiu: fresh=false → STALE_STOP (fecha + alerta)
    LevelInput stale{true, true, false};
    TEST_ASSERT_EQUAL(Act::STALE_STOP, step(e, t, stale, 40000).act);
    // ainda stale, já alertado e já desligado → NONE
    TEST_ASSERT_EQUAL(Act::NONE, step(e, t, stale, 45000).act);
    // volta fresco e baixo, minOff cumprido desde a parada(40s) → START de novo
    TEST_ASSERT_EQUAL(Act::START, step(e, t, freshInput(true), 80000).act);
}

static void test_absent_snapshot_treated_as_stale()
{
    LevelControlEngine e;
    LevelControlTable t;
    t.upsert(rule1());
    step(e, t, freshInput(true), 31000);              // START
    LevelInput absent{false, false, false};
    TEST_ASSERT_EQUAL(Act::STALE_STOP, step(e, t, absent, 35000).act);
}

static void test_polarity_inverted()
{
    LevelControlEngine e;
    LevelControlTable t;
    LevelRule r = rule1();
    r.ligaQuandoAtivo = false; // liga quando INATIVO
    t.upsert(r);
    // ativo agora significa "não ligar"
    TEST_ASSERT_EQUAL(Act::NONE, step(e, t, freshInput(true), 31000).act);
    // inativo → quer ligar; minOff ok → START
    TEST_ASSERT_EQUAL(Act::START, step(e, t, freshInput(false), 62000).act);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_starts_when_low_after_minOff);
    RUN_TEST(test_stops_when_full_after_minOn);
    RUN_TEST(test_renew_while_on);
    RUN_TEST(test_stale_stops_and_alerts_once);
    RUN_TEST(test_absent_snapshot_treated_as_stale);
    RUN_TEST(test_polarity_inverted);
    exit(UNITY_END());
}
void loop() {}
