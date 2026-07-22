#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/InterlockTable.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_upsert_and_byid()
{
    InterlockTable t;
    InterlockRule r;
    r.id = 3; r.tipo = IL_SENSOR; r.node = 0xA1B2C3D4; r.sensorIdx = 1;
    r.condicao = COND_MENOR_QUE; r.valorCenti = 150; r.acao = ACAO_FECHAR_E_BLOQUEAR;
    r.zoneIds[0] = 2; r.zoneIds[1] = 5;
    TEST_ASSERT_TRUE(t.upsert(r));
    TEST_ASSERT_EQUAL_UINT32(1, t.count());
    const InterlockRule *g = t.byId(3);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4, g->node);
    TEST_ASSERT_EQUAL_UINT8(5, g->zoneIds[1]);
}

static void test_remove_and_full()
{
    InterlockTable t;
    for (uint8_t i = 1; i <= InterlockTable::MAX; i++) {
        InterlockRule r; r.id = i; r.tipo = IL_SIMULTANEIDADE; r.maxAbertas = 2;
        TEST_ASSERT_TRUE(t.upsert(r));
    }
    InterlockRule extra; extra.id = 99;
    TEST_ASSERT_FALSE(t.upsert(extra)); // cheia
    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_TRUE(t.upsert(extra));  // agora cabe
}

static void test_roundtrip_serialize()
{
    InterlockTable t;
    InterlockRule r; r.id = 7; r.tipo = IL_SENSOR; r.todas = true;
    r.condicao = COND_ATIVO; r.acao = ACAO_BLOQUEAR_ABERTURA;
    memcpy(r.mensagem, "nivel baixo", 12);
    t.upsert(r);
    uint8_t buf[1024];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    InterlockTable t2;
    TEST_ASSERT_TRUE(t2.deserialize(buf, n));
    const InterlockRule *g = t2.byId(7);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_TRUE(g->todas);
    TEST_ASSERT_EQUAL_STRING("nivel baixo", g->mensagem);
}

static void test_corrupt_deserialize_empties()
{
    InterlockTable t;
    uint8_t junk[16] = {0xDE, 0xAD};
    TEST_ASSERT_FALSE(t.deserialize(junk, sizeof(junk)));
    TEST_ASSERT_EQUAL_UINT32(0, t.count());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_upsert_and_byid);
    RUN_TEST(test_remove_and_full);
    RUN_TEST(test_roundtrip_serialize);
    RUN_TEST(test_corrupt_deserialize_empties);
    exit(UNITY_END());
}
void loop() {}
