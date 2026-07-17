#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/PortalSession.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static const uint32_t TEN_MIN = 10u * 60u * 1000u;

static void test_startsClosed()
{
    PortalSession s;
    TEST_ASSERT_FALSE(s.apShouldBeUp());
    TEST_ASSERT_EQUAL_UINT32(0, s.secondsLeft(0));
}

static void test_requestOpen_raisesAp()
{
    PortalSession s;
    s.requestOpen(1000);
    TEST_ASSERT_TRUE(s.apShouldBeUp());
    TEST_ASSERT_EQUAL_UINT32(600, s.secondsLeft(1000)); // 10 min
}

static void test_idleTimeout_closes()
{
    PortalSession s;
    s.requestOpen(0);
    s.tick(TEN_MIN - 1);
    TEST_ASSERT_TRUE(s.apShouldBeUp());
    s.tick(TEN_MIN); // 10 min sem cliente
    TEST_ASSERT_FALSE(s.apShouldBeUp());
}

static void test_clientPresence_renews()
{
    PortalSession s;
    s.requestOpen(0);
    s.noteClient(TEN_MIN - 1, true); // cliente conectado renova a atividade
    s.tick(TEN_MIN);
    TEST_ASSERT_TRUE(s.apShouldBeUp()); // não fechou: atividade recente
    s.tick((TEN_MIN - 1) + TEN_MIN);    // 10 min após a última atividade
    TEST_ASSERT_FALSE(s.apShouldBeUp());
}

static void test_reopenAfterClose()
{
    PortalSession s;
    s.requestOpen(0);
    s.tick(TEN_MIN);
    TEST_ASSERT_FALSE(s.apShouldBeUp());
    s.requestOpen(TEN_MIN + 5000);
    TEST_ASSERT_TRUE(s.apShouldBeUp());
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_startsClosed);
    RUN_TEST(test_requestOpen_raisesAp);
    RUN_TEST(test_idleTimeout_closes);
    RUN_TEST(test_clientPresence_renews);
    RUN_TEST(test_reopenAfterClose);
    UNITY_END();
}

void loop() {}
