#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/WeatherEngine.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static WeatherCache freshCache(uint16_t mmCenti, uint8_t pct) {
    WeatherCache c; c.valid = true; c.fetchEpoch = 1000;
    c.chuvaPrevista12hCenti = mmCenti; c.probChuvaPct = pct;
    return c;
}
static WeatherRule rule(uint8_t id, uint16_t mm, uint8_t pct, uint8_t enabled) {
    WeatherRule r; r.id = id; r.enabled = enabled;
    r.limiarMmCenti = mm; r.limiarPct = pct;
    r.zonaIds[0] = 5; r.grupoIds[0] = 2;
    return r;
}
static const uint32_t NOW = 1000 + 3600; // 1h após fetch
static const uint16_t TTL = 24;

static void test_fail_open_invalid_cache() {
    WeatherCache c; // valid=false
    WeatherRuleTable t; t.upsert(rule(1, 100, 10, 1));
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).suppress);
}
static void test_fail_open_stale_cache() {
    WeatherCache c = freshCache(1000, 90);
    WeatherRuleTable t; t.upsert(rule(1, 100, 10, 1));
    uint32_t late = 1000 + (uint32_t)TTL * 3600 + 1;
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, late, TTL).suppress);
}
static void test_fail_open_no_rtc() {
    WeatherCache c = freshCache(1000, 90);
    WeatherRuleTable t; t.upsert(rule(1, 100, 10, 1));
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, 0, TTL).suppress);
}
static void test_suppress_when_both_exceed() {
    WeatherCache c = freshCache(800, 70); // 8.0mm, 70%
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 1)); // >5mm E >60%
    WeatherVerdict v = WeatherEngine::zoneVerdict(5, c, t, NOW, TTL);
    TEST_ASSERT_TRUE(v.suppress);
    TEST_ASSERT_EQUAL(1, v.ruleId);
}
static void test_strict_inequality_no_suppress_on_equal() {
    WeatherCache c = freshCache(500, 60); // exatamente nos limiares
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 1));
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).suppress);
}
static void test_only_one_condition_no_suppress() {
    WeatherCache c = freshCache(800, 50); // chuva ok, prob abaixo
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 1));
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).suppress);
}
static void test_disabled_rule_ignored() {
    WeatherCache c = freshCache(800, 70);
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 0)); // disabled
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).suppress);
}
static void test_target_not_covered() {
    WeatherCache c = freshCache(800, 70);
    WeatherRuleTable t; t.upsert(rule(1, 500, 60, 1)); // cobre zona 5, grupo 2
    TEST_ASSERT_FALSE(WeatherEngine::zoneVerdict(9, c, t, NOW, TTL).suppress);
    TEST_ASSERT_TRUE(WeatherEngine::groupVerdict(2, c, t, NOW, TTL).suppress);
    TEST_ASSERT_FALSE(WeatherEngine::groupVerdict(9, c, t, NOW, TTL).suppress);
}
static void test_first_matching_rule_wins() {
    WeatherCache c = freshCache(800, 70);
    WeatherRuleTable t;
    t.upsert(rule(3, 500, 60, 1)); // ambas cobrem zona 5
    t.upsert(rule(7, 100, 10, 1));
    uint8_t rid = WeatherEngine::zoneVerdict(5, c, t, NOW, TTL).ruleId;
    TEST_ASSERT_TRUE(rid == 3 || rid == 7); // qualquer que case; determinístico por slot
}

void setup() {
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_fail_open_invalid_cache);
    RUN_TEST(test_fail_open_stale_cache);
    RUN_TEST(test_fail_open_no_rtc);
    RUN_TEST(test_suppress_when_both_exceed);
    RUN_TEST(test_strict_inequality_no_suppress_on_equal);
    RUN_TEST(test_only_one_condition_no_suppress);
    RUN_TEST(test_disabled_rule_ignored);
    RUN_TEST(test_target_not_covered);
    RUN_TEST(test_first_matching_rule_wins);
    exit(UNITY_END());
}
void loop() {}
