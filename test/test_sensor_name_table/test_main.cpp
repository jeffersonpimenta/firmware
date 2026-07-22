#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/SensorNameTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_set_get_update()
{
    SensorNameTable t;
    TEST_ASSERT_TRUE(t.set(0xAA, 1, "pressao"));
    TEST_ASSERT_EQUAL_STRING("pressao", t.get(0xAA, 1));
    TEST_ASSERT_NULL(t.get(0xAA, 2));
    TEST_ASSERT_TRUE(t.set(0xAA, 1, "pressao linha")); // update
    TEST_ASSERT_EQUAL_STRING("pressao linha", t.get(0xAA, 1));
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
}

static void test_full_rejects()
{
    SensorNameTable t;
    for (uint32_t i = 1; i <= SensorNameTable::MAX; i++)
        TEST_ASSERT_TRUE(t.set(i, 0, "x"));
    TEST_ASSERT_FALSE(t.set(999, 0, "y")); // cheia
    // update de entrada existente ainda funciona mesmo cheia:
    TEST_ASSERT_TRUE(t.set(1, 0, "z"));
}

static void test_roundtrip()
{
    SensorNameTable t; t.set(0xBB, 0, "nivel");
    uint8_t buf[2048];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    SensorNameTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_STRING("nivel", t2.get(0xBB, 0));
}

static void test_corrupt_empties()
{
    SensorNameTable t;
    uint8_t junk[8] = {1, 2, 3};
    TEST_ASSERT_FALSE(t.deserialize(junk, sizeof(junk)));
    TEST_ASSERT_EQUAL_UINT32(0, t.count());
}

static void test_name_truncates_to_15_chars()
{
    SensorNameTable t;
    t.set(0xCC, 0, "0123456789ABCDEFGHIJ"); // 20 chars
    const char *g = t.get(0xCC, 0);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT32(15, strlen(g)); // 15 + NUL em char[16]
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_set_get_update);
    RUN_TEST(test_full_rejects);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_corrupt_empties);
    RUN_TEST(test_name_truncates_to_15_chars);
    exit(UNITY_END());
}
void loop() {}
