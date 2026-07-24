#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/HydraulicGroupTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static HydraulicGroup mk(uint8_t id, uint8_t pump, std::initializer_list<uint8_t> zs)
{
    HydraulicGroup g;
    g.id = id;
    g.bombaZoneId = pump;
    g.zoneCount = 0;
    for (uint8_t z : zs)
        g.zoneIds[g.zoneCount++] = z;
    g.minOpen = 1;
    g.maxOpen = 2;
    return g;
}

static void test_upsert_e_lookup()
{
    HydraulicGroupTable t;
    TEST_ASSERT_TRUE(t.upsert(mk(1, 9, {1, 2, 3})));
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
    TEST_ASSERT_NOT_NULL(t.byId(1));
    TEST_ASSERT_EQUAL_UINT8(9, t.byId(1)->bombaZoneId);
    TEST_ASSERT_EQUAL_UINT8(1, t.byZone(2)->id);      // zona-membro -> grupo
    TEST_ASSERT_EQUAL_UINT8(1, t.byPumpZone(9)->id);  // bomba -> grupo
    TEST_ASSERT_NULL(t.byZone(9));                     // bomba não é membro
    TEST_ASSERT_NULL(t.byId(2));
}

static void test_zona_em_dois_grupos_rejeitada()
{
    HydraulicGroupTable t;
    TEST_ASSERT_TRUE(t.upsert(mk(1, 9, {1, 2})));
    TEST_ASSERT_FALSE(t.upsert(mk(2, 8, {2, 3}))); // zona 2 já usada
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
}

static void test_validacoes()
{
    HydraulicGroupTable t;
    HydraulicGroup g = mk(1, 9, {1, 2});
    g.minOpen = 3; // > zoneCount(2)
    TEST_ASSERT_FALSE(t.upsert(g));
    g.minOpen = 1;
    g.maxOpen = 0; // sem teto ok
    TEST_ASSERT_TRUE(t.upsert(g));
    HydraulicGroup h = mk(2, 5, {5, 6}); // bomba 5 é membro do próprio grupo
    TEST_ASSERT_FALSE(t.upsert(h));
    HydraulicGroup z = mk(0, 9, {7}); // id 0 inválido
    TEST_ASSERT_FALSE(t.upsert(z));
}

static void test_serialize_roundtrip()
{
    HydraulicGroupTable a;
    a.upsert(mk(1, 9, {1, 2, 3}));
    a.upsert(mk(2, 0, {4, 5})); // grupo sem bomba
    uint8_t buf[600];
    size_t n = a.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    HydraulicGroupTable b;
    TEST_ASSERT_TRUE(b.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT32(2, b.count());
    TEST_ASSERT_EQUAL_UINT8(9, b.byId(1)->bombaZoneId);
    TEST_ASSERT_EQUAL_UINT8(0, b.byId(2)->bombaZoneId);
    TEST_ASSERT_EQUAL_UINT8(3, b.byId(1)->zoneCount);
}

static void test_deserialize_corrompido_zera()
{
    HydraulicGroupTable a;
    a.upsert(mk(1, 9, {1}));
    uint8_t buf[600];
    size_t n = a.serialize(buf, sizeof(buf));
    buf[0] ^= 0xFF; // corrompe MAGIC
    HydraulicGroupTable b;
    b.upsert(mk(7, 3, {7}));
    TEST_ASSERT_FALSE(b.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT32(0, b.count()); // falha => vazia
}

static void test_remove()
{
    HydraulicGroupTable t;
    t.upsert(mk(1, 9, {1, 2}));
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_FALSE(t.removeById(1));
    TEST_ASSERT_EQUAL_UINT32(0, t.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_upsert_e_lookup);
    RUN_TEST(test_zona_em_dois_grupos_rejeitada);
    RUN_TEST(test_validacoes);
    RUN_TEST(test_serialize_roundtrip);
    RUN_TEST(test_deserialize_corrompido_zera);
    RUN_TEST(test_remove);
    exit(UNITY_END());
}
void loop() {}
