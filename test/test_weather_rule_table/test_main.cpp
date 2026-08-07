#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/WeatherRuleTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static WeatherRule mkRule(uint8_t id) {
    WeatherRule r; r.id = id; r.enabled = 1;
    r.limiarMmCenti = 500; r.limiarPct = 60;
    r.zonaIds[0] = 3; r.grupoIds[0] = 1;
    strncpy(r.nome, "Chuva forte", WeatherRule::NOME_LEN - 1);
    strncpy(r.mensagem, "Chuva prevista 12h", WeatherRule::MSG_LEN - 1);
    return r;
}

static void test_upsert_byid_count() {
    WeatherRuleTable t;
    TEST_ASSERT_TRUE(t.upsert(mkRule(1)));
    TEST_ASSERT_TRUE(t.upsert(mkRule(2)));
    TEST_ASSERT_EQUAL(2, t.count());
    const WeatherRule *g = t.byId(1);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL(500, g->limiarMmCenti);
    TEST_ASSERT_EQUAL_STRING("Chuva forte", g->nome);
}

static void test_covers() {
    WeatherRule r = mkRule(1);
    TEST_ASSERT_TRUE(r.coversZone(3));
    TEST_ASSERT_FALSE(r.coversZone(9));
    TEST_ASSERT_TRUE(r.coversGroup(1));
    TEST_ASSERT_FALSE(r.coversGroup(2));
}

static void test_remove_and_full() {
    WeatherRuleTable t;
    for (uint8_t i = 1; i <= WeatherRuleTable::MAX; i++) TEST_ASSERT_TRUE(t.upsert(mkRule(i)));
    TEST_ASSERT_FALSE(t.upsert(mkRule(99))); // cheia
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_NULL(t.byId(1));
    TEST_ASSERT_TRUE(t.upsert(mkRule(99))); // abriu vaga
}

static void test_roundtrip() {
    WeatherRuleTable t;
    t.upsert(mkRule(1)); t.upsert(mkRule(2));
    uint8_t buf[2048];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    WeatherRuleTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL(2, t2.count());
    TEST_ASSERT_EQUAL_STRING("Chuva prevista 12h", t2.byId(2)->mensagem);
}

static void test_corrupt_empties() {
    uint8_t buf[16] = {0};
    WeatherRuleTable t; t.upsert(mkRule(1));
    TEST_ASSERT_FALSE(t.deserialize(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, t.count());
}

void setup() {
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_upsert_byid_count);
    RUN_TEST(test_covers);
    RUN_TEST(test_remove_and_full);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_corrupt_empties);
    exit(UNITY_END());
}
void loop() {}
