#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationWebApi.h"
#include "modules/irrigation/WeatherEngine.h"
#include <string.h>
#include <unity.h>

using namespace IrrigationWeb;

void setUp(void) {}
void tearDown(void) {}

static void test_build_status_has_fields()
{
    WeatherConfig cfg;
    cfg.enabled = 1;
    cfg.latE7 = -235000000;
    cfg.lonE7 = -466000000;
    WeatherCache cache;
    cache.valid = true;
    cache.fetchEpoch = 1000;
    cache.chuvaPrevista12hCenti = 850;
    cache.probChuvaPct = 62;
    WeatherRuleTable rules;
    WeatherRule r;
    r.id = 1;
    r.enabled = 1;
    r.limiarMmCenti = 500;
    r.limiarPct = 60;
    r.grupoIds[0] = 1;
    strncpy(r.nome, "Chuva forte", 31);
    rules.upsert(r);
    WeatherStatusCtx ctx;
    ctx.cfg = &cfg;
    ctx.cache = &cache;
    ctx.rules = &rules;
    ctx.nowEpoch = 1000 + 3600;
    ctx.staUp = true;
    ctx.location = "Casa";
    char buf[2048];
    size_t n = buildWeatherStatus(ctx, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"enabled\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"probChuva\":62"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"triggered\":true")); // 8.5 > 5 e 62 > 60
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"nome\":\"Chuva forte\""));
}

static void test_parse_rule_ok()
{
    const char *body = "{\"nome\":\"Horta\",\"limiarMm\":3,\"limiarPct\":50,"
                       "\"zonaIds\":[1,2],\"grupoIds\":[],\"enabled\":true,"
                       "\"mensagem\":\"dispensa horta\"}";
    WeatherRuleParse p = parseWeatherRule(body, strlen(body));
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL(300, p.rule.limiarMmCenti); // 3.0mm → 300 centi
    TEST_ASSERT_EQUAL(50, p.rule.limiarPct);
    TEST_ASSERT_EQUAL(1, p.rule.zonaIds[0]);
    TEST_ASSERT_EQUAL(2, p.rule.zonaIds[1]);
    TEST_ASSERT_EQUAL_STRING("Horta", p.rule.nome);
}

static void test_parse_rule_requires_target()
{
    const char *body = "{\"nome\":\"X\",\"limiarMm\":3,\"limiarPct\":50,"
                       "\"zonaIds\":[],\"grupoIds\":[]}";
    WeatherRuleParse p = parseWeatherRule(body, strlen(body));
    TEST_ASSERT_FALSE(p.ok);
}

static void test_parse_rule_requires_name()
{
    const char *body = "{\"nome\":\"\",\"limiarMm\":3,\"limiarPct\":50,\"zonaIds\":[1]}";
    WeatherRuleParse p = parseWeatherRule(body, strlen(body));
    TEST_ASSERT_FALSE(p.ok);
}

static void test_parse_config_ok()
{
    const char *body = "{\"enabled\":true,\"lat\":-23.5,\"lon\":-46.6}";
    WeatherConfigParse p = parseWeatherConfig(body, strlen(body));
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL(1, p.enabled);
    TEST_ASSERT_EQUAL_INT32(-235000000, p.latE7);
    TEST_ASSERT_EQUAL_INT32(-466000000, p.lonE7);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_build_status_has_fields);
    RUN_TEST(test_parse_rule_ok);
    RUN_TEST(test_parse_rule_requires_target);
    RUN_TEST(test_parse_rule_requires_name);
    RUN_TEST(test_parse_config_ok);
    exit(UNITY_END());
}
void loop() {}
