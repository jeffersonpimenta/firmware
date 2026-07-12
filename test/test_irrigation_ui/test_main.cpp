#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ButtonGesture.h"
#include "modules/irrigation/LedPattern.h"
#include <unity.h>

using Ev = ButtonGestureDetector::Event;
using Mode = LedPatternController::Mode;

void setUp(void) {}
void tearDown(void) {}

// Simula amostragem de 25 ms: pressiona em [t0, t1), retorna eventos coletados.
static void sample(ButtonGestureDetector &d, uint32_t from, uint32_t to, bool pressed, Ev *outEv, int &count)
{
    for (uint32_t t = from; t < to; t += 25) {
        Ev e = d.update(pressed, t);
        if (e != Ev::NONE && count < 8)
            outEv[count++] = e;
    }
}

static void test_shortPress_emitsShortAfterGap()
{
    ButtonGestureDetector d;
    Ev evs[8];
    int n = 0;
    sample(d, 0, 200, true, evs, n);    // pressiona 200 ms
    sample(d, 200, 700, false, evs, n); // solta; gap de dupla expira em 600
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(Ev::SHORT, evs[0]);
}

static void test_doublePress_emitsDouble()
{
    ButtonGestureDetector d;
    Ev evs[8];
    int n = 0;
    sample(d, 0, 150, true, evs, n);
    sample(d, 150, 300, false, evs, n); // solta 150 ms
    sample(d, 300, 450, true, evs, n);  // segunda pressão dentro dos 400 ms
    sample(d, 450, 1000, false, evs, n);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL(Ev::DOUBLE, evs[0]);
}

static void test_longPress_firesOnceAt3s_thenHoldAt10s()
{
    ButtonGestureDetector d;
    Ev evs[8];
    int n = 0;
    sample(d, 0, 11000, true, evs, n); // segura 11 s
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL(Ev::LONG_3S, evs[0]);
    TEST_ASSERT_EQUAL(Ev::HOLD_10S, evs[1]);
    sample(d, 11000, 12000, false, evs, n); // soltar não emite mais nada
    TEST_ASSERT_EQUAL_INT(2, n);
}

static void test_bounceIgnored()
{
    ButtonGestureDetector d;
    Ev evs[8];
    int n = 0;
    // pulso de 25 ms (uma amostra) — abaixo do debounce de 30 ms
    d.update(true, 0);
    sample(d, 25, 800, false, evs, n);
    TEST_ASSERT_EQUAL_INT(0, n);
}

static void test_led_normal_oneBlinkPer5s()
{
    LedPatternController led;
    led.setMode(Mode::NORMAL);
    TEST_ASSERT_TRUE(led.ledOn(5000 + 50));    // 1ª piscada da janela
    TEST_ASSERT_FALSE(led.ledOn(5000 + 200));  // gap
    TEST_ASSERT_FALSE(led.ledOn(5000 + 300));  // sem 2ª piscada
    TEST_ASSERT_FALSE(led.ledOn(5000 + 4000)); // resto da janela apagado
}

static void test_led_noGateway_twoBlinks_configPending_three()
{
    LedPatternController led;
    led.setMode(Mode::NO_GATEWAY);
    TEST_ASSERT_TRUE(led.ledOn(50));   // piscada 1 [0,100)
    TEST_ASSERT_TRUE(led.ledOn(300));  // piscada 2 [250,350)
    TEST_ASSERT_FALSE(led.ledOn(550)); // sem piscada 3
    led.setMode(Mode::CONFIG_PENDING);
    TEST_ASSERT_TRUE(led.ledOn(550)); // piscada 3 [500,600)
}

static void test_led_pairing_fastBlink_and_open_solid()
{
    LedPatternController led;
    led.setMode(Mode::PAIRING);
    TEST_ASSERT_TRUE(led.ledOn(50));   // [0,100) on
    TEST_ASSERT_FALSE(led.ledOn(150)); // [100,200) off
    TEST_ASSERT_TRUE(led.ledOn(250));
    led.setMode(Mode::OUTPUT_OPEN);
    TEST_ASSERT_TRUE(led.ledOn(0));
    TEST_ASSERT_TRUE(led.ledOn(123456));
}

static void test_led_sos_shortShortLong()
{
    LedPatternController led;
    led.setMode(Mode::BATTERY_SOS);
    TEST_ASSERT_TRUE(led.ledOn(50));    // ponto 1 [0,150)
    TEST_ASSERT_FALSE(led.ledOn(200));  // gap
    TEST_ASSERT_TRUE(led.ledOn(350));   // ponto 2 [300,450)
    TEST_ASSERT_TRUE(led.ledOn(1000));  // traço 1 [900,1350)
    TEST_ASSERT_FALSE(led.ledOn(4500)); // pausa final da janela
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_shortPress_emitsShortAfterGap);
    RUN_TEST(test_doublePress_emitsDouble);
    RUN_TEST(test_longPress_firesOnceAt3s_thenHoldAt10s);
    RUN_TEST(test_bounceIgnored);
    RUN_TEST(test_led_normal_oneBlinkPer5s);
    RUN_TEST(test_led_noGateway_twoBlinks_configPending_three);
    RUN_TEST(test_led_pairing_fastBlink_and_open_solid);
    RUN_TEST(test_led_sos_shortShortLong);
    exit(UNITY_END());
}

void loop() {}
