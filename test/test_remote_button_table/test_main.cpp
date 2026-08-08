#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/RemoteButtonTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static RemoteAssoc mkAssoc(uint8_t id, uint8_t zoneId, uint32_t n0, uint8_t in0)
{
    RemoteAssoc a;
    a.id = id;
    a.enabled = 1;
    a.targetZoneId = zoneId;
    a.triggers[0] = {n0, in0, 0};
    return a;
}

static void test_upsert_lookup_count()
{
    RemoteButtonTable t;
    TEST_ASSERT_TRUE(t.upsert(mkAssoc(1, 5, 0xAA, 2)));
    TEST_ASSERT_TRUE(t.upsert(mkAssoc(2, 6, 0xBB, 0)));
    TEST_ASSERT_EQUAL_UINT(2, t.count());
    RemoteAssoc a1b = mkAssoc(1, 9, 0xCC, 1); // mesmo id substitui
    TEST_ASSERT_TRUE(t.upsert(a1b));
    TEST_ASSERT_EQUAL_UINT(2, t.count());
    TEST_ASSERT_EQUAL_UINT8(9, t.byId(1)->targetZoneId);
    TEST_ASSERT_NULL(t.byId(99));
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_NULL(t.byId(1));
}

static void test_findByTrigger_multi()
{
    RemoteButtonTable t;
    RemoteAssoc a = mkAssoc(1, 5, 0xAA, 2);
    a.triggers[1] = {0xBB, 3, 1}; // segunda botoeira, outro nó
    t.upsert(a);
    t.upsert(mkAssoc(2, 5, 0xBB, 3)); // outra assoc, MESMO gatilho (0xBB,3) → mesma saída
    const RemoteAssoc *out[RemoteButtonTable::MAX];
    size_t k = t.findByTrigger(0xBB, 3, out, RemoteButtonTable::MAX);
    TEST_ASSERT_EQUAL_UINT(2, k); // ambas casam
    TEST_ASSERT_EQUAL_UINT(0, t.findByTrigger(0x99, 0, out, RemoteButtonTable::MAX));
    TEST_ASSERT_EQUAL_UINT8(2, t.byId(1)->triggerCount());
}

static void test_full_and_idzero_rejected()
{
    RemoteButtonTable t;
    for (uint8_t i = 1; i <= RemoteButtonTable::MAX; i++)
        TEST_ASSERT_TRUE(t.upsert(mkAssoc(i, i, i, 0)));
    TEST_ASSERT_FALSE(t.upsert(mkAssoc(200, 1, 1, 0)));
    TEST_ASSERT_FALSE(t.upsert(mkAssoc(0, 1, 1, 0)));
}

static void test_serialize_round_trip()
{
    RemoteButtonTable t;
    RemoteAssoc a = mkAssoc(1, 5, 0xAA, 2);
    a.triggers[1] = {0xBB, 3, 1};
    a.enabled = 0;
    t.upsert(a);
    uint8_t buf[512];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    RemoteButtonTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(1, t2.count());
    const RemoteAssoc *r = t2.byId(1);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8(0, r->enabled);
    TEST_ASSERT_EQUAL_HEX32(0xBB, r->triggers[1].node);
    TEST_ASSERT_EQUAL_UINT8(1, r->triggers[1].ledSlot);
    TEST_ASSERT_FALSE(t2.deserialize(buf, 3)); // curto → tabela vazia
    TEST_ASSERT_EQUAL_UINT(0, t2.count());
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_upsert_lookup_count);
    RUN_TEST(test_findByTrigger_multi);
    RUN_TEST(test_full_and_idzero_rejected);
    RUN_TEST(test_serialize_round_trip);
    exit(UNITY_END());
}

void loop() {}
