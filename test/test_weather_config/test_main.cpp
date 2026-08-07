#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/WeatherConfig.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_roundtrip() {
    WeatherConfig c;
    c.enabled = 1; c.latE7 = -235000000; c.lonE7 = -466000000;
    c.pollHourA = 5; c.pollHourB = 17; c.staleTtlH = 12;
    uint8_t buf[64];
    size_t n = c.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(WeatherConfig::SERIALIZED, n);
    WeatherConfig c2;
    TEST_ASSERT_TRUE(c2.deserialize(buf, n));
    TEST_ASSERT_EQUAL(1, c2.enabled);
    TEST_ASSERT_EQUAL_INT32(-235000000, c2.latE7);
    TEST_ASSERT_EQUAL_INT32(-466000000, c2.lonE7);
    TEST_ASSERT_EQUAL(5, c2.pollHourA);
    TEST_ASSERT_EQUAL(17, c2.pollHourB);
    TEST_ASSERT_EQUAL(12, c2.staleTtlH);
}

static void test_bad_magic_keeps_defaults() {
    uint8_t buf[WeatherConfig::SERIALIZED] = {0};
    WeatherConfig c; c.pollHourA = 9;
    TEST_ASSERT_FALSE(c.deserialize(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(9, c.pollHourA); // inalterado em falha
}

static void test_bad_crc_rejected() {
    WeatherConfig c; c.enabled = 1;
    uint8_t buf[64];
    size_t n = c.serialize(buf, sizeof(buf));
    buf[7] ^= 0xFF; // corrompe payload
    WeatherConfig c2;
    TEST_ASSERT_FALSE(c2.deserialize(buf, n));
}

void setup() {
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_bad_magic_keeps_defaults);
    RUN_TEST(test_bad_crc_rejected);
    exit(UNITY_END());
}
void loop() {}
