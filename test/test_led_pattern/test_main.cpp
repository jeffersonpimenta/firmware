#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/LedPattern.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// CMD_INFLIGHT: pisca rapido, periodo 150 ms.
// Deve alternar estado entre t=0 e t=160 (mais de meio periodo).
// ---------------------------------------------------------------------------
static void test_cmd_inflight_blinks_fast()
{
    LedPatternController c;
    c.setMode(LedPatternController::Mode::CMD_INFLIGHT);
    bool a = c.ledOn(0);
    bool b = c.ledOn(160);
    TEST_ASSERT_NOT_EQUAL(a, b); // estado diferente depois de 160 ms
    // Dentro de 600 ms deve haver pelo menos uma transicao adicional
    bool c0 = c.ledOn(300);
    bool c1 = c.ledOn(460);
    TEST_ASSERT_NOT_EQUAL(c0, c1);
}

// ---------------------------------------------------------------------------
// CMD_OK: 2 pulsos curtos por ciclo de 1000 ms.
// Padrão: ligado [0,120), desligado [120,240), ligado [240,360), desligado resto.
// ---------------------------------------------------------------------------
static void test_cmd_ok_pattern_at_key_instants()
{
    LedPatternController c;
    c.setMode(LedPatternController::Mode::CMD_OK);

    // Pulso 1: [0, 120)
    TEST_ASSERT_TRUE(c.ledOn(0));
    TEST_ASSERT_TRUE(c.ledOn(60));
    TEST_ASSERT_TRUE(c.ledOn(119));

    // Gap entre pulsos: [120, 240)
    TEST_ASSERT_FALSE(c.ledOn(120));
    TEST_ASSERT_FALSE(c.ledOn(180));
    TEST_ASSERT_FALSE(c.ledOn(239));

    // Pulso 2: [240, 360)
    TEST_ASSERT_TRUE(c.ledOn(240));
    TEST_ASSERT_TRUE(c.ledOn(300));
    TEST_ASSERT_TRUE(c.ledOn(359));

    // Apagado no resto do ciclo: [360, 1000)
    TEST_ASSERT_FALSE(c.ledOn(360));
    TEST_ASSERT_FALSE(c.ledOn(500));
    TEST_ASSERT_FALSE(c.ledOn(999));

    // Padrao repete no ciclo seguinte
    TEST_ASSERT_TRUE(c.ledOn(1000));
    TEST_ASSERT_FALSE(c.ledOn(1120));
    TEST_ASSERT_TRUE(c.ledOn(1240));
    TEST_ASSERT_FALSE(c.ledOn(1360));
}

// ---------------------------------------------------------------------------
// CMD_FAIL: rajada de 3 pulsos por ciclo de 1200 ms.
// Padrão: on [0,100), off [100,200), on [200,300), off [300,400), on [400,500), off resto.
// ---------------------------------------------------------------------------
static void test_cmd_fail_burst_pattern_at_key_instants()
{
    LedPatternController c;
    c.setMode(LedPatternController::Mode::CMD_FAIL);

    // Pulso 1: [0, 100)
    TEST_ASSERT_TRUE(c.ledOn(0));
    TEST_ASSERT_TRUE(c.ledOn(50));
    TEST_ASSERT_TRUE(c.ledOn(99));

    // Gap 1: [100, 200)
    TEST_ASSERT_FALSE(c.ledOn(100));
    TEST_ASSERT_FALSE(c.ledOn(150));
    TEST_ASSERT_FALSE(c.ledOn(199));

    // Pulso 2: [200, 300)
    TEST_ASSERT_TRUE(c.ledOn(200));
    TEST_ASSERT_TRUE(c.ledOn(250));
    TEST_ASSERT_TRUE(c.ledOn(299));

    // Gap 2: [300, 400)
    TEST_ASSERT_FALSE(c.ledOn(300));
    TEST_ASSERT_FALSE(c.ledOn(350));
    TEST_ASSERT_FALSE(c.ledOn(399));

    // Pulso 3: [400, 500)
    TEST_ASSERT_TRUE(c.ledOn(400));
    TEST_ASSERT_TRUE(c.ledOn(450));
    TEST_ASSERT_TRUE(c.ledOn(499));

    // Apagado no resto do ciclo: [500, 1200)
    TEST_ASSERT_FALSE(c.ledOn(500));
    TEST_ASSERT_FALSE(c.ledOn(800));
    TEST_ASSERT_FALSE(c.ledOn(1199));

    // Padrao repete no ciclo seguinte
    TEST_ASSERT_TRUE(c.ledOn(1200));
    TEST_ASSERT_FALSE(c.ledOn(1300));
    TEST_ASSERT_TRUE(c.ledOn(1400));
}

// ---------------------------------------------------------------------------
// Funcao pura: setMode nao altera nowMs — os padroes sao funcoes do tempo.
// ---------------------------------------------------------------------------
static void test_cmd_modes_are_pure_functions_of_time()
{
    LedPatternController c;

    // CMD_INFLIGHT: mesma entrada -> mesmo resultado
    c.setMode(LedPatternController::Mode::CMD_INFLIGHT);
    TEST_ASSERT_EQUAL(c.ledOn(75), c.ledOn(75));

    // CMD_OK: idem
    c.setMode(LedPatternController::Mode::CMD_OK);
    TEST_ASSERT_EQUAL(c.ledOn(50), c.ledOn(50));
    TEST_ASSERT_EQUAL(c.ledOn(500), c.ledOn(500));

    // CMD_FAIL: idem
    c.setMode(LedPatternController::Mode::CMD_FAIL);
    TEST_ASSERT_EQUAL(c.ledOn(50), c.ledOn(50));
    TEST_ASSERT_EQUAL(c.ledOn(600), c.ledOn(600));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_cmd_inflight_blinks_fast);
    RUN_TEST(test_cmd_ok_pattern_at_key_instants);
    RUN_TEST(test_cmd_fail_burst_pattern_at_key_instants);
    RUN_TEST(test_cmd_modes_are_pure_functions_of_time);
    exit(UNITY_END());
}
void loop() {}
