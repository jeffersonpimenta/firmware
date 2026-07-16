#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationWebApi.h"
#include <string.h>
#include <unity.h>

using namespace IrrigationWeb;

void setUp(void) {}
void tearDown(void) {}

static bool contains(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }

static void test_computeSync_states()
{
    TEST_ASSERT_EQUAL(SyncState::INALCANCAVEL, computeSync(5, 5, true));
    TEST_ASSERT_EQUAL(SyncState::SINCRONIZADA, computeSync(5, 5, false));
    TEST_ASSERT_EQUAL(SyncState::PENDENTE, computeSync(6, 5, false));
    TEST_ASSERT_EQUAL(SyncState::PENDENTE, computeSync(1, 0, false));
}

static void test_buildOverview_json()
{
    OverviewCtx c = {};
    c.hasRtc = false;
    c.stationCount = 3;
    c.running = 0;
    c.alertCount = 2;
    c.pairingPending = true;
    c.pairingNodeId = 0xABCD;
    c.pairingSecondsLeft = 90;
    char buf[512];
    size_t n = buildOverview(c, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(contains(buf, "\"hasRtc\":false"));
    TEST_ASSERT_TRUE(contains(buf, "\"stationCount\":3"));
    TEST_ASSERT_TRUE(contains(buf, "\"alertCount\":2"));
    TEST_ASSERT_TRUE(contains(buf, "\"pairingPending\":true"));
    TEST_ASSERT_TRUE(contains(buf, "\"pairingNodeId\":43981")); // 0xABCD
}

static void test_buildOverview_truncationReturnsZero()
{
    OverviewCtx c = {};
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(0, buildOverview(c, buf, sizeof(buf)));
}

// Arrays de escalares precisam de vírgula entre elementos (str/num/boolean chamam sep_).
static void test_jsonWriter_scalarArrayCommas()
{
    char buf[128];
    JsonWriter w(buf, sizeof(buf));
    w.beginArray();
    w.str("a");
    w.str("b");
    w.endArray();
    TEST_ASSERT_EQUAL_STRING("[\"a\",\"b\"]", buf);

    JsonWriter w2(buf, sizeof(buf));
    w2.beginArray();
    w2.num(1);
    w2.num(2);
    w2.endArray();
    TEST_ASSERT_EQUAL_STRING("[1,2]", buf);
}

// str() escapa aspas/barra/newline e descarta controle.
static void test_jsonWriter_strEscaping()
{
    char buf[64];
    // "\x01" separado de "e" por concatenação: senão \x01e vira um único hex 0x1E (greedy).
    JsonWriter w(buf, sizeof(buf));
    w.str("a\"b\\c\nd\x01"
          "e");
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\nde\"", buf);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_computeSync_states);
    RUN_TEST(test_buildOverview_json);
    RUN_TEST(test_buildOverview_truncationReturnsZero);
    RUN_TEST(test_jsonWriter_scalarArrayCommas);
    RUN_TEST(test_jsonWriter_strEscaping);
    exit(UNITY_END());
}

void loop() {}
