#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/LevelControlTable.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static LevelRule mkRule(uint8_t id, uint32_t node, uint8_t target)
{
    LevelRule r{};
    r.id = id;
    r.sensorNode = node;
    r.sensorIdx = 1;
    r.ligaQuandoAtivo = true;
    r.targetZoneId = target;
    r.minOnS = 30;
    r.minOffS = 30;
    r.staleTimeoutS = 90;
    strncpy(r.mensagem, "cisterna", sizeof(r.mensagem) - 1);
    return r;
}

static void test_upsert_and_byId()
{
    LevelControlTable t;
    TEST_ASSERT_TRUE(t.upsert(mkRule(1, 0xaabbccdd, 5)));
    TEST_ASSERT_EQUAL_size_t(1, t.count());
    const LevelRule *r = t.byId(1);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_HEX32(0xaabbccdd, r->sensorNode);
    TEST_ASSERT_EQUAL_UINT8(5, r->targetZoneId);
    TEST_ASSERT_NULL(t.byId(2));
    TEST_ASSERT_FALSE(t.upsert(mkRule(0, 1, 1))); // id 0 recusado
}

static void test_upsert_updates_in_place()
{
    LevelControlTable t;
    t.upsert(mkRule(1, 1, 5));
    LevelRule r = mkRule(1, 1, 9); // mesmo id, target novo
    TEST_ASSERT_TRUE(t.upsert(r));
    TEST_ASSERT_EQUAL_size_t(1, t.count());
    TEST_ASSERT_EQUAL_UINT8(9, t.byId(1)->targetZoneId);
}

static void test_full_table_rejects()
{
    LevelControlTable t;
    for (uint8_t i = 1; i <= LevelControlTable::MAX; i++)
        TEST_ASSERT_TRUE(t.upsert(mkRule(i, i, i)));
    TEST_ASSERT_FALSE(t.upsert(mkRule(99, 1, 1))); // cheia
}

static void test_remove()
{
    LevelControlTable t;
    t.upsert(mkRule(1, 1, 5));
    t.upsert(mkRule(2, 2, 6));
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_EQUAL_size_t(1, t.count());
    TEST_ASSERT_NULL(t.byId(1));
    TEST_ASSERT_FALSE(t.removeById(1)); // já removido
}

static void test_serialize_roundtrip()
{
    LevelControlTable t;
    t.upsert(mkRule(1, 0x11223344, 5));
    t.upsert(mkRule(3, 0x55667788, 7));
    uint8_t buf[256];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    LevelControlTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(2, t2.count());
    TEST_ASSERT_EQUAL_HEX32(0x55667788, t2.byId(3)->sensorNode);
}

static void test_deserialize_bad_crc_empties()
{
    LevelControlTable t;
    t.upsert(mkRule(1, 1, 5));
    uint8_t buf[256];
    size_t n = t.serialize(buf, sizeof(buf));
    buf[8] ^= 0xff; // corrompe um byte de dado
    LevelControlTable t2;
    TEST_ASSERT_FALSE(t2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(0, t2.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_upsert_and_byId);
    RUN_TEST(test_upsert_updates_in_place);
    RUN_TEST(test_full_table_rejects);
    RUN_TEST(test_remove);
    RUN_TEST(test_serialize_roundtrip);
    RUN_TEST(test_deserialize_bad_crc_empties);
    exit(UNITY_END());
}
void loop() {}
