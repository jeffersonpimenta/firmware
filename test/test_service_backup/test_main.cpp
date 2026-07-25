#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ServiceBackup.h"
#include <cstring>
#include <unity.h>

using namespace IrrigationService;

void setUp(void) {}
void tearDown(void) {}

static void test_b64_roundtrip()
{
    const uint8_t raw[] = {0xd4, 0xf1, 0xbb, 0x3a};
    char out[16];
    size_t n = base64Encode(raw, 4, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(8, n);
    out[n] = 0;
    TEST_ASSERT_EQUAL_STRING("1PG7Og==", out);
    uint8_t back[8];
    int m = base64Decode(out, n, back, sizeof back);
    TEST_ASSERT_EQUAL_INT(4, m);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(raw, back, 4);
}

static void test_b64_encode_overflow_returns_zero()
{
    const uint8_t raw[] = {1, 2, 3};
    char out[3];
    TEST_ASSERT_EQUAL_size_t(0, base64Encode(raw, 3, out, sizeof out)); // needs 4
}

static void test_b64_decode_rejects_bad_char()
{
    uint8_t back[8];
    TEST_ASSERT_EQUAL_INT(-1, base64Decode("!!!!", 4, back, sizeof back));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_b64_roundtrip);
    RUN_TEST(test_b64_encode_overflow_returns_zero);
    RUN_TEST(test_b64_decode_rejects_bad_char);
    exit(UNITY_END());
}

void loop() {}
