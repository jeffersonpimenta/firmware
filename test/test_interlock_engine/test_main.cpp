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

static void test_engine_duas_regras_mesma_zona_combina()
{
    // Uma BLOQUEAR_ABERTURA + uma FECHAR_E_BLOQUEAR, ambas disparando na zona 4.
    InterlockTable tbl;
    InterlockRule a; a.id = 1; a.tipo = IL_SENSOR; a.node = 0xAA; a.sensorIdx = 0;
    a.condicao = COND_ATIVO; a.acao = ACAO_BLOQUEAR_ABERTURA; a.zoneIds[0] = 4;
    InterlockRule b; b.id = 2; b.tipo = IL_SENSOR; b.node = 0xBB; b.sensorIdx = 1;
    b.condicao = COND_ATIVO; b.acao = ACAO_FECHAR_E_BLOQUEAR; b.zoneIds[0] = 4;
    tbl.upsert(a); tbl.upsert(b);
    InterlockEngine eng;
    SensorSnapshot s[2] = { {0xAA, 0, true, true, 0}, {0xBB, 1, true, true, 0} };
    eng.evaluate(tbl, s, 2);
    ZoneVerdict v = eng.zoneVerdict(4);
    TEST_ASSERT_TRUE(v.bloqueada);
    TEST_ASSERT_TRUE(v.deveFechar); // acumula as duas regras (sem early-return)
}

static void test_engine_latch_persiste_entre_ticks()
{
    // Histerese: dispara <150, tick seguinte com valor na banda [150,160) segue latched.
    InterlockTable tbl;
    InterlockRule r; r.id = 1; r.tipo = IL_SENSOR; r.node = 0xAA; r.sensorIdx = 0;
    r.condicao = COND_MENOR_QUE; r.valorCenti = 150; r.histereseCenti = 10;
    r.acao = ACAO_FECHAR_E_BLOQUEAR; r.zoneIds[0] = 4;
    tbl.upsert(r);
    InterlockEngine eng;
    SensorSnapshot lo{0xAA, 0, true, false, 149};
    eng.evaluate(tbl, &lo, 1);
    TEST_ASSERT_TRUE(eng.zoneVerdict(4).deveFechar); // disparou
    SensorSnapshot band{0xAA, 0, true, false, 155};
    eng.evaluate(tbl, &band, 1);
    TEST_ASSERT_TRUE(eng.zoneVerdict(4).deveFechar); // banda: latch persiste
    SensorSnapshot hi{0xAA, 0, true, false, 160};
    eng.evaluate(tbl, &hi, 1);
    TEST_ASSERT_FALSE(eng.zoneVerdict(4).deveFechar); // desarmou
}

static void test_engine_multiplos_caps_menor_vence()
{
    InterlockTable tbl;
    InterlockRule a; a.id = 1; a.tipo = IL_SIMULTANEIDADE; a.maxAbertas = 3;
    InterlockRule b; b.id = 2; b.tipo = IL_SIMULTANEIDADE; b.maxAbertas = 2;
    InterlockRule c; c.id = 3; c.tipo = IL_SIMULTANEIDADE; c.maxAbertas = 0; // ignorado
    tbl.upsert(a); tbl.upsert(b); tbl.upsert(c);
    InterlockEngine eng;
    TEST_ASSERT_EQUAL_UINT8(2, eng.evaluate(tbl, nullptr, 0)); // menor cap positivo
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
    RUN_TEST(test_engine_duas_regras_mesma_zona_combina);
    RUN_TEST(test_engine_latch_persiste_entre_ticks);
    RUN_TEST(test_engine_multiplos_caps_menor_vence);
    exit(UNITY_END());
}
void loop() {}
