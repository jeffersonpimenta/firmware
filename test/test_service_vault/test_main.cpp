#include "Arduino.h"
#include "TestUtil.h"
#include "support/RamProfileStore.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_ramstore_write_read_list()
{
    RamProfileStore s;
    TEST_ASSERT_TRUE(s.writeProfile("f1", "{\"id\":\"f1\"}", 11));
    char buf[64];
    size_t n = 0;
    TEST_ASSERT_TRUE(s.readProfile("f1", buf, sizeof buf, n));
    TEST_ASSERT_EQUAL_size_t(11, n);
    char ids[8][32];
    TEST_ASSERT_EQUAL_size_t(1, s.listIds(ids, 8));
    TEST_ASSERT_EQUAL_STRING("f1", ids[0]);
}

static void test_ramstore_active_roundtrip()
{
    RamProfileStore s;
    TEST_ASSERT_TRUE(s.setActive("f2"));
    char a[32];
    TEST_ASSERT_TRUE(s.getActive(a, sizeof a));
    TEST_ASSERT_EQUAL_STRING("f2", a);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_ramstore_write_read_list);
    RUN_TEST(test_ramstore_active_roundtrip);
    exit(UNITY_END());
}
void loop() {}
