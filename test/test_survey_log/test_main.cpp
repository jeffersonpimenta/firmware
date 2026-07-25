#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/SurveyLog.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static SurveyPoint pt(uint32_t node)
{
    SurveyPoint p{};
    p.node = node;
    return p;
}

static void test_log_add_and_order()
{
    SurveyLog log;
    log.add(pt(1));
    log.add(pt(2));
    log.add(pt(3));
    TEST_ASSERT_EQUAL_size_t(3, log.count());
    TEST_ASSERT_EQUAL_UINT32(1, log.at(0).node); // mais antigo
    TEST_ASSERT_EQUAL_UINT32(3, log.at(2).node); // mais novo
}

static void test_log_overflow_evicts_oldest()
{
    SurveyLog log;
    for (uint32_t i = 1; i <= SurveyLog::CAP + 2; i++)
        log.add(pt(i));
    TEST_ASSERT_EQUAL_size_t(SurveyLog::CAP, log.count());
    TEST_ASSERT_EQUAL_UINT32(3, log.at(0).node); // 1 e 2 despejados
    TEST_ASSERT_EQUAL_UINT32(SurveyLog::CAP + 2, log.at(SurveyLog::CAP - 1).node);
}

static void test_log_clear()
{
    SurveyLog log;
    log.add(pt(1));
    log.clear();
    TEST_ASSERT_EQUAL_size_t(0, log.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_log_add_and_order);
    RUN_TEST(test_log_overflow_evicts_oldest);
    RUN_TEST(test_log_clear);
    exit(UNITY_END());
}
void loop() {}
