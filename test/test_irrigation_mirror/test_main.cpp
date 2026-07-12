#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/MirrorMode.h"
#include <unity.h>

using T = MirrorMode::Action::T;

void setUp(void) {}
void tearDown(void) {}

// leva o debounce: aplica bitmap por >=100 ms com chamadas a cada 25 ms
static MirrorMode::Action settle(MirrorMode &m, uint8_t bm, uint32_t from)
{
    MirrorMode::Action last;
    for (uint32_t t = from; t <= from + 125; t += 25) {
        auto a = m.update(bm, t);
        if (a.t != T::NONE)
            last = a;
    }
    return last;
}

static void test_riseOpensFallCloses()
{
    MirrorMode m;
    m.setEnabled(true);
    auto a = settle(m, 0b0001, 1000);
    TEST_ASSERT_EQUAL(T::OPEN, a.t);
    TEST_ASSERT_EQUAL_UINT8(0, a.input);
    TEST_ASSERT_TRUE(m.inputActive(0));
    auto c = settle(m, 0b0000, 5000);
    TEST_ASSERT_EQUAL(T::CLOSE, c.t);
    TEST_ASSERT_FALSE(m.inputActive(0));
}

static void test_renewalEvery60s()
{
    MirrorMode m;
    m.setEnabled(true);
    settle(m, 0b0010, 1000);
    TEST_ASSERT_EQUAL(T::NONE, m.update(0b0010, 30000).t);
    auto r = m.update(0b0010, 1000 + 125 + 60000);
    TEST_ASSERT_EQUAL(T::OPEN, r.t); // renovação
    TEST_ASSERT_EQUAL_UINT8(1, r.input);
}

static void test_glitchIgnored()
{
    MirrorMode m;
    m.setEnabled(true);
    m.update(0b0001, 1000);
    m.update(0b0000, 1050); // caiu antes de 100 ms
    TEST_ASSERT_EQUAL(T::NONE, m.update(0b0000, 1200).t);
    TEST_ASSERT_FALSE(m.inputActive(0));
}

static void test_disableDrainsActiveInputs()
{
    MirrorMode m;
    m.setEnabled(true);
    settle(m, 0b0101, 1000); // pode vir OPEN de 0 e 2 em chamadas sucessivas
    settle(m, 0b0101, 2000);
    m.setEnabled(false);
    auto c1 = m.update(0b0101, 3000);
    auto c2 = m.update(0b0101, 3000);
    bool closed0 = (c1.t == T::CLOSE && c1.input == 0) || (c2.t == T::CLOSE && c2.input == 0);
    bool closed2 = (c1.t == T::CLOSE && c1.input == 2) || (c2.t == T::CLOSE && c2.input == 2);
    TEST_ASSERT_TRUE(closed0);
    TEST_ASSERT_TRUE(closed2);
    TEST_ASSERT_EQUAL(T::NONE, m.update(0b0101, 3100).t); // desabilitado: ignora entradas
}

static void test_twoInputsIndependent()
{
    MirrorMode m;
    m.setEnabled(true);
    auto a = settle(m, 0b1000, 1000);
    TEST_ASSERT_EQUAL_UINT8(3, a.input);
    // entra a 0 também; 3 continua
    MirrorMode::Action b = settle(m, 0b1001, 2000);
    TEST_ASSERT_EQUAL(T::OPEN, b.t);
    TEST_ASSERT_EQUAL_UINT8(0, b.input);
    TEST_ASSERT_TRUE(m.inputActive(3));
    auto c = settle(m, 0b0001, 4000); // solta 3
    TEST_ASSERT_EQUAL(T::CLOSE, c.t);
    TEST_ASSERT_EQUAL_UINT8(3, c.input);
    TEST_ASSERT_TRUE(m.inputActive(0));
}

static void test_serializeEnabledFlag()
{
    MirrorMode m;
    m.setEnabled(true);
    uint8_t buf[8];
    size_t n = m.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    MirrorMode c;
    TEST_ASSERT_TRUE(c.deserialize(buf, n));
    TEST_ASSERT_TRUE(c.enabled());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_riseOpensFallCloses);
    RUN_TEST(test_renewalEvery60s);
    RUN_TEST(test_glitchIgnored);
    RUN_TEST(test_disableDrainsActiveInputs);
    RUN_TEST(test_twoInputsIndependent);
    RUN_TEST(test_serializeEnabledFlag);
    exit(UNITY_END());
}

void loop() {}
