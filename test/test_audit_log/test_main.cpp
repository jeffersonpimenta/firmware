#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/AuditLog.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// Cria um registro simples com timestamp e ação ABRIR para uso nos testes.
static AuditRecord rec(uint32_t ts)
{
    AuditRecord r;
    r.tsSecs = ts;
    r.action = (uint8_t)AuditAction::ABRIR;
    return r;
}

// Verifica comportamento do ring: wrapping e ordem (mais recente em at(0)).
static void test_ring_wrap_and_order()
{
    AuditRecord store[4];
    AuditLog log(store, 4);
    for (uint32_t i = 1; i <= 6; i++)
        log.append(rec(i));
    TEST_ASSERT_EQUAL_size_t(4, log.size());
    TEST_ASSERT_EQUAL_UINT32(6, log.at(0).tsSecs); // mais recente primeiro
    TEST_ASSERT_EQUAL_UINT32(3, log.at(3).tsSecs);
}

// Verifica serialização/deserialização e rejeição de CRC corrompido.
static void test_serialize_roundtrip_and_crc()
{
    AuditRecord a[8], b[8];
    AuditLog la(a, 8), lb(b, 8);
    la.append(rec(10));
    la.append(rec(20));
    uint8_t buf[256];
    size_t n = la.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(lb.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(2, lb.size());
    TEST_ASSERT_EQUAL_UINT32(20, lb.at(0).tsSecs);
    buf[n - 1] ^= 0xff; // corrompe CRC
    TEST_ASSERT_FALSE(lb.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(2, lb.size()); // intacto após falha
}

// Verifica que deserializar em buffer menor preserva os registros mais recentes.
static void test_deserialize_smaller_cap_keeps_recent()
{
    AuditRecord a[8], c[2];
    AuditLog la(a, 8), lc(c, 2);
    for (uint32_t i = 1; i <= 5; i++)
        la.append(rec(i));
    uint8_t buf[256];
    size_t n = la.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(lc.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(2, lc.size());
    TEST_ASSERT_EQUAL_UINT32(5, lc.at(0).tsSecs);
    TEST_ASSERT_EQUAL_UINT32(4, lc.at(1).tsSecs);
}

// Verifica que clear() zera o log e que log vazio serializa/deserializa corretamente.
static void test_clear_empties()
{
    AuditRecord store[4];
    AuditLog log(store, 4);
    log.append(rec(100));
    log.append(rec(200));
    TEST_ASSERT_EQUAL_size_t(2, log.size());
    log.clear();
    TEST_ASSERT_EQUAL_size_t(0, log.size());

    // Log vazio deve serializar e deserializar sem erros.
    uint8_t buf[256];
    size_t n = log.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    AuditRecord store2[4];
    AuditLog log2(store2, 4);
    TEST_ASSERT_TRUE(log2.deserialize(buf, n));
    TEST_ASSERT_EQUAL_size_t(0, log2.size());
}

// Verifica que serialize retorna 0 quando o buffer é pequeno demais,
// e que deserialize rejeita buffers inválidos (curtos ou lixo).
static void test_serialize_too_small_returns_zero()
{
    AuditRecord store[4];
    AuditLog log(store, 4);
    log.append(rec(1));
    log.append(rec(2));

    // Buffer menor que o necessário: deve retornar 0.
    uint8_t tiny[4];
    size_t n = log.serialize(tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_size_t(0, n);

    // Deserializar buffer muito curto deve retornar false sem alterar estado.
    AuditRecord store2[4];
    AuditLog log2(store2, 4);
    log2.append(rec(99));
    uint8_t garbage[] = {0x00, 0x01, 0x02};
    TEST_ASSERT_FALSE(log2.deserialize(garbage, sizeof(garbage)));
    // Conteúdo anterior deve estar intacto.
    TEST_ASSERT_EQUAL_size_t(1, log2.size());
    TEST_ASSERT_EQUAL_UINT32(99, log2.at(0).tsSecs);

    // Deserializar buffer com magic errado deve retornar false.
    uint8_t badMagic[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_FALSE(log2.deserialize(badMagic, sizeof(badMagic)));
    TEST_ASSERT_EQUAL_size_t(1, log2.size()); // intacto
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_ring_wrap_and_order);
    RUN_TEST(test_serialize_roundtrip_and_crc);
    RUN_TEST(test_deserialize_smaller_cap_keeps_recent);
    RUN_TEST(test_clear_empties);
    RUN_TEST(test_serialize_too_small_returns_zero);
    exit(UNITY_END());
}

void loop() {}
