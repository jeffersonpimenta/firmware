#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/InterlockEngine.h"
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

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_digital_ativo);
    RUN_TEST(test_analog_menor_que_com_histerese);
    RUN_TEST(test_analog_maior_que_com_histerese);
    exit(UNITY_END());
}
void loop() {}
