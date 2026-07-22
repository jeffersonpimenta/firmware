#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/OpenGate.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_admite_ate_cap_depois_enfileira()
{
    OpenGate g; g.setCap(2);
    TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(1, 60));
    TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(2, 60));
    TEST_ASSERT_EQUAL(OpenGate::Decision::HOLD, g.request(3, 90)); // cheio -> fila
    TEST_ASSERT_TRUE(g.isQueued(3));
    TEST_ASSERT_EQUAL_UINT32(2, g.openCount());
}

static void test_release_libera_fila_em_ordem()
{
    OpenGate g; g.setCap(1);
    g.request(1, 60);
    g.request(2, 70); // fila
    g.request(3, 80); // fila
    TEST_ASSERT_EQUAL_UINT8(0, g.nextAdmittable().zoneId); // sem capacidade ainda
    g.release(1);
    OpenGate::Pending p = g.nextAdmittable();
    TEST_ASSERT_EQUAL_UINT8(2, p.zoneId); // FIFO: 2 antes de 3
    TEST_ASSERT_EQUAL_UINT16(70, p.durationS); // carrega a duração enfileirada
    TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(2, 70)); // glue readmite
    g.release(2);
    TEST_ASSERT_EQUAL_UINT8(3, g.nextAdmittable().zoneId);
}

static void test_cap_zero_sem_limite()
{
    OpenGate g; g.setCap(0);
    for (uint8_t i = 1; i <= 10; i++)
        TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(i, 60));
    TEST_ASSERT_EQUAL_UINT32(10, g.openCount());
}

static void test_request_zona_ja_aberta_idempotente()
{
    OpenGate g; g.setCap(2);
    g.request(1, 60);
    TEST_ASSERT_EQUAL(OpenGate::Decision::ADMIT, g.request(1, 60)); // renovação, não conta 2x
    TEST_ASSERT_EQUAL_UINT32(1, g.openCount());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_admite_ate_cap_depois_enfileira);
    RUN_TEST(test_release_libera_fila_em_ordem);
    RUN_TEST(test_cap_zero_sem_limite);
    RUN_TEST(test_request_zona_ja_aberta_idempotente);
    exit(UNITY_END());
}
void loop() {}
