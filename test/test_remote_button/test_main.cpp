#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/RemoteButtonEdge.h"
#include "modules/irrigation/RemoteLedFsm.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_edge_debounce_risingOnce()
{
    RemoteButtonEdge e;
    TEST_ASSERT_FALSE(e.update(false, 0));
    TEST_ASSERT_FALSE(e.update(true, 10));   // subiu mas dentro do debounce
    TEST_ASSERT_FALSE(e.update(true, 40));   // ainda instável (< 50ms)
    TEST_ASSERT_TRUE(e.update(true, 70));    // estável → 1 borda
    TEST_ASSERT_FALSE(e.update(true, 200));  // segurando: sem repetir
    TEST_ASSERT_FALSE(e.update(false, 260)); // soltou (debounce)
    TEST_ASSERT_FALSE(e.update(false, 320)); // estável baixo, rearma
    TEST_ASSERT_TRUE(e.update(true, 400));   // nova subida (após rearmar)
}

static void test_led_fsm()
{
    RemoteLedFsm f;
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::OFF, f.state(0));
    f.onPress(1000);
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::BLINK, f.state(1000));
    // pisca: alterna com o tempo
    bool a = f.ledOn(1000), b = f.ledOn(1000 + RemoteLedFsm::BLINK_PERIOD_MS);
    TEST_ASSERT_NOT_EQUAL(a, b);
    f.onLedState(true, 1200); // push do gateway = ligado
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::SOLID, f.state(1200));
    TEST_ASSERT_TRUE(f.ledOn(9999));
    f.onLedState(false, 1300); // saída desligou
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::OFF, f.state(1300));
    // timeout: press sem push → apaga
    f.onPress(2000);
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::BLINK, f.state(2000));
    TEST_ASSERT_EQUAL(RemoteLedFsm::State::OFF, f.state(2000 + RemoteLedFsm::BLINK_TIMEOUT_MS + 1));
}

static void test_toggle_action()
{
    TEST_ASSERT_EQUAL_UINT8(1, remoteToggleAction(false)); // desligado → abrir
    TEST_ASSERT_EQUAL_UINT8(0, remoteToggleAction(true));  // ligado → fechar
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_edge_debounce_risingOnce);
    RUN_TEST(test_led_fsm);
    RUN_TEST(test_toggle_action);
    return UNITY_END();
}
