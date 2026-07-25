#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ServiceBackup.h"
#include "modules/irrigation/ServiceController.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_channelFromProfile_decodes()
{
    IrrigationService::LightProfile p{};
    strcpy(p.canalNome, "bv-irrig");
    strcpy(p.pskB64, "1PG7Og==");
    p.preset = 1;
    RetunePlan r = channelFromProfile(p);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("bv-irrig", r.name);
    TEST_ASSERT_EQUAL_size_t(4, r.pskLen);
    TEST_ASSERT_EQUAL_UINT8(1, r.preset);
}

static void test_channelFromProfile_clamps_long_name()
{
    IrrigationService::LightProfile p{};
    strcpy(p.canalNome, "abc");
    strcpy(p.pskB64, "1PG7Og==");
    RetunePlan r = channelFromProfile(p);
    TEST_ASSERT_TRUE(strlen(r.name) <= 12);
}

static void test_channelFromProfile_bad_psk_not_ok()
{
    IrrigationService::LightProfile p{};
    strcpy(p.canalNome, "c");
    strcpy(p.pskB64, "!!!!");
    RetunePlan r = channelFromProfile(p);
    TEST_ASSERT_FALSE(r.ok);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_channelFromProfile_decodes);
    RUN_TEST(test_channelFromProfile_clamps_long_name);
    RUN_TEST(test_channelFromProfile_bad_psk_not_ok);
    exit(UNITY_END());
}
void loop() {}
