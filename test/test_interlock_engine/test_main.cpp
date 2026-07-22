#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/InterlockEngine.h"
#include "modules/irrigation/InterlockTable.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_digital_ativo()
{
    bool latch = false;
    TEST_ASSERT_TRUE(evalCondition(COND_ATIVO, true, 0, 0, 0, latch));
    TEST_ASSERT_FALSE(evalCondition(COND_ATIVO, false, 0, 0, 0, latch));
}

static void test_analog_menor_que_com_histerese()
{
    // limiar 150 (1,50), histerese 10 (0,10): dispara <150, só desarma >=160.
    bool latch = false;
    TEST_ASSERT_FALSE(evalCondition(COND_MENOR_QUE, false, 200, 150, 10, latch));
    TEST_ASSERT_TRUE(evalCondition(COND_MENOR_QUE, false, 149, 150, 10, latch));  // dispara
    TEST_ASSERT_TRUE(evalCondition(COND_MENOR_QUE, false, 155, 150, 10, latch));  // banda: segue latched
    TEST_ASSERT_FALSE(evalCondition(COND_MENOR_QUE, false, 160, 150, 10, latch)); // desarma
}

static void test_analog_maior_que_com_histerese()
{
    bool latch = false;
    TEST_ASSERT_TRUE(evalCondition(COND_MAIOR_QUE, false, 300, 250, 20, latch));  // >250 dispara
    TEST_ASSERT_TRUE(evalCondition(COND_MAIOR_QUE, false, 235, 250, 20, latch));  // banda [230,250]: latched
    TEST_ASSERT_FALSE(evalCondition(COND_MAIOR_QUE, false, 229, 250, 20, latch)); // <230 desarma
}

static void test_engine_fecha_e_bloqueia_zona()
{
    InterlockTable tbl;
    InterlockRule r; r.id = 1; r.tipo = IL_SENSOR; r.node = 0xAA; r.sensorIdx = 0;
    r.condicao = COND_MENOR_QUE; r.valorCenti = 150; r.histereseCenti = 10;
    r.acao = ACAO_FECHAR_E_BLOQUEAR; r.zoneIds[0] = 4;
    tbl.upsert(r);
    InterlockEngine eng;
    SensorSnapshot s{0xAA, 0, true, false, 149};
    eng.evaluate(tbl, &s, 1);
    ZoneVerdict v = eng.zoneVerdict(4);
    TEST_ASSERT_TRUE(v.deveFechar);
    TEST_ASSERT_TRUE(v.bloqueada);
    TEST_ASSERT_EQUAL_UINT8(1, v.ruleId);
    TEST_ASSERT_FALSE(eng.zoneVerdict(9).bloqueada); // zona não coberta
}

static void test_engine_todas_zonas_e_bloquear_abertura()
{
    InterlockTable tbl;
    InterlockRule r; r.id = 2; r.tipo = IL_SENSOR; r.node = 0xBB; r.sensorIdx = 1;
    r.condicao = COND_ATIVO; r.acao = ACAO_BLOQUEAR_ABERTURA; r.todas = true;
    tbl.upsert(r);
    InterlockEngine eng;
    SensorSnapshot s{0xBB, 1, true, true, 0};
    eng.evaluate(tbl, &s, 1);
    ZoneVerdict v = eng.zoneVerdict(123);
    TEST_ASSERT_TRUE(v.bloqueada);
    TEST_ASSERT_FALSE(v.deveFechar); // bloquear_abertura não fecha o que já está aberto
}

static void test_engine_cap_simultaneidade()
{
    InterlockTable tbl;
    InterlockRule r; r.id = 3; r.tipo = IL_SIMULTANEIDADE; r.maxAbertas = 2;
    tbl.upsert(r);
    InterlockEngine eng;
    TEST_ASSERT_EQUAL_UINT8(2, eng.evaluate(tbl, nullptr, 0));
}

static void test_engine_sensor_ausente_nao_dispara()
{
    InterlockTable tbl;
    InterlockRule r; r.id = 1; r.tipo = IL_SENSOR; r.node = 0xAA; r.sensorIdx = 0;
    r.condicao = COND_ATIVO; r.acao = ACAO_FECHAR_E_BLOQUEAR; r.todas = true;
    tbl.upsert(r);
    InterlockEngine eng;
    SensorSnapshot s{0xAA, 0, false, true, 0}; // present=false
    eng.evaluate(tbl, &s, 1);
    TEST_ASSERT_FALSE(eng.zoneVerdict(1).deveFechar);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_digital_ativo);
    RUN_TEST(test_analog_menor_que_com_histerese);
    RUN_TEST(test_analog_maior_que_com_histerese);
    RUN_TEST(test_engine_fecha_e_bloqueia_zona);
    RUN_TEST(test_engine_todas_zonas_e_bloquear_abertura);
    RUN_TEST(test_engine_cap_simultaneidade);
    RUN_TEST(test_engine_sensor_ausente_nao_dispara);
    exit(UNITY_END());
}
void loop() {}
