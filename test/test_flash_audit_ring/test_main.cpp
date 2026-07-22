#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/FlashAuditRing.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include <string.h>
#include <unity.h>
#include <vector>

struct MemStore : IByteStore {
    std::vector<uint8_t> bytes;
    explicit MemStore(size_t n) : bytes(n, 0) {}
    size_t size() const override { return bytes.size(); }
    bool read(size_t off, void *dst, size_t n) const override {
        if (off + n > bytes.size()) return false;
        memcpy(dst, bytes.data() + off, n); return true;
    }
    bool write(size_t off, const void *src, size_t n) override {
        if (off + n > bytes.size()) return false;
        memcpy(bytes.data() + off, src, n); return true;
    }
};

static AuditRecord rec(uint32_t ts) { AuditRecord r; r.tsSecs = ts; r.action = 1; return r; }

void setUp(void) {}
void tearDown(void) {}

static void test_append_and_newest_first()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC); // cap 4
    FlashAuditRing ring(s);
    TEST_ASSERT_TRUE(ring.begin());
    ring.append(rec(10)); ring.append(rec(20)); ring.append(rec(30));
    TEST_ASSERT_EQUAL_UINT32(3, ring.count());
    AuditRecord g;
    TEST_ASSERT_TRUE(ring.at(0, g)); TEST_ASSERT_EQUAL_UINT32(30, g.tsSecs); // mais recente
    TEST_ASSERT_TRUE(ring.at(2, g)); TEST_ASSERT_EQUAL_UINT32(10, g.tsSecs);
}

static void test_wrap_keeps_recent()
{
    MemStore s(FlashAuditRing::HEADER + 3 * FlashAuditRing::REC); // cap 3
    FlashAuditRing ring(s); ring.begin();
    for (uint32_t i = 1; i <= 5; i++) ring.append(rec(i * 10)); // 10..50, cap 3
    TEST_ASSERT_EQUAL_UINT32(3, ring.count());
    AuditRecord g;
    ring.at(0, g); TEST_ASSERT_EQUAL_UINT32(50, g.tsSecs);
    ring.at(2, g); TEST_ASSERT_EQUAL_UINT32(30, g.tsSecs); // 10 e 20 perdidos
}

static void test_persists_across_reopen()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    { FlashAuditRing ring(s); ring.begin(); ring.append(rec(77)); }
    FlashAuditRing ring2(s);
    TEST_ASSERT_TRUE(ring2.begin()); // header válido
    TEST_ASSERT_EQUAL_UINT32(1, ring2.count());
    AuditRecord g; ring2.at(0, g); TEST_ASSERT_EQUAL_UINT32(77, g.tsSecs);
}

static void test_corrupt_header_formats()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    s.bytes[0] = 0xFF; // magic inválido
    FlashAuditRing ring(s);
    TEST_ASSERT_TRUE(ring.begin()); // formata em vez de falhar
    TEST_ASSERT_EQUAL_UINT32(0, ring.count());
}

static void test_csv_header_and_rows()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    FlashAuditRing ring(s); ring.begin();
    AuditRecord r; r.tsSecs = 100; r.origin = 1; r.action = 0; r.target = 3;
    r.result = 0; r.node = 0xABCD; r.seq = 9;
    ring.append(r);
    char out[512];
    size_t n = ring.toCsv(out, sizeof(out), 50);
    TEST_ASSERT_GREATER_THAN(0, n);
    out[n] = 0;
    TEST_ASSERT_NOT_NULL(strstr(out, "ts,origem,acao,alvo,resultado,node,seq")); // cabeçalho
    TEST_ASSERT_NOT_NULL(strstr(out, "100,"));
}

static void test_json_array()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    FlashAuditRing ring(s); ring.begin();
    ring.append(rec(42));
    char out[512];
    size_t n = ring.toJson(out, sizeof(out), 50);
    out[n] = 0;
    TEST_ASSERT_EQUAL_CHAR('[', out[0]);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"ts\":42"));
}

static void test_json_vazio()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    FlashAuditRing ring(s); ring.begin();
    char out[8];
    size_t n = ring.toJson(out, sizeof(out), 50);
    out[n] = 0;
    TEST_ASSERT_EQUAL_STRING("[]", out);
}

static void test_csv_cap_pequeno_devolve_zero()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    FlashAuditRing ring(s); ring.begin();
    ring.append(rec(1));
    char out[10];
    TEST_ASSERT_EQUAL_UINT32(0, ring.toCsv(out, sizeof(out), 50)); // não cabe nem o cabeçalho
}

static void test_json_trunca_em_registro_inteiro()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    FlashAuditRing ring(s); ring.begin();
    ring.append(rec(11)); ring.append(rec(22));
    char out[64]; // cabe '[' + 1 objeto + ']' mas não os 2
    size_t n = ring.toJson(out, sizeof(out), 50);
    out[n] = 0;
    TEST_ASSERT_EQUAL_CHAR('[', out[0]);
    TEST_ASSERT_EQUAL_CHAR(']', out[n - 1]); // JSON fechado, não cortado no meio
}

static void test_crc_invalido_formata()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    { FlashAuditRing ring(s); ring.begin(); ring.append(rec(5)); }
    s.bytes[12] ^= 0xFF; // corrompe só o CRC (magic intacto)
    FlashAuditRing ring2(s);
    TEST_ASSERT_TRUE(ring2.begin());
    TEST_ASSERT_EQUAL_UINT32(0, ring2.count()); // reformatou
}

static void test_head_fora_de_faixa_formata()
{
    MemStore s(FlashAuditRing::HEADER + 3 * FlashAuditRing::REC); // cap 3
    { FlashAuditRing ring(s); ring.begin(); ring.append(rec(5)); }
    uint32_t bad = 99; memcpy(s.bytes.data() + 4, &bad, 4); // head >= capacity
    // recomputa o CRC p/ passar da checagem de CRC e cair na checagem de faixa
    uint32_t crc = IrrigationProto::crc32(s.bytes.data(), 12);
    memcpy(s.bytes.data() + 12, &crc, 4);
    FlashAuditRing ring2(s);
    TEST_ASSERT_TRUE(ring2.begin());
    TEST_ASSERT_EQUAL_UINT32(0, ring2.count());
}

static void test_wrap_persiste_apos_reopen()
{
    MemStore s(FlashAuditRing::HEADER + 3 * FlashAuditRing::REC); // cap 3
    { FlashAuditRing ring(s); ring.begin(); for (uint32_t i = 1; i <= 5; i++) ring.append(rec(i * 10)); }
    FlashAuditRing ring2(s);
    TEST_ASSERT_TRUE(ring2.begin());
    TEST_ASSERT_EQUAL_UINT32(3, ring2.count());
    AuditRecord g; ring2.at(0, g); TEST_ASSERT_EQUAL_UINT32(50, g.tsSecs); // mais recente sobrevive
}

static void test_at_fora_de_faixa_false()
{
    MemStore s(FlashAuditRing::HEADER + 4 * FlashAuditRing::REC);
    FlashAuditRing ring(s); ring.begin(); ring.append(rec(1));
    AuditRecord g;
    TEST_ASSERT_FALSE(ring.at(1, g)); // i >= count
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_append_and_newest_first);
    RUN_TEST(test_wrap_keeps_recent);
    RUN_TEST(test_persists_across_reopen);
    RUN_TEST(test_corrupt_header_formats);
    RUN_TEST(test_csv_header_and_rows);
    RUN_TEST(test_json_array);
    RUN_TEST(test_json_vazio);
    RUN_TEST(test_csv_cap_pequeno_devolve_zero);
    RUN_TEST(test_json_trunca_em_registro_inteiro);
    RUN_TEST(test_crc_invalido_formata);
    RUN_TEST(test_head_fora_de_faixa_formata);
    RUN_TEST(test_wrap_persiste_apos_reopen);
    RUN_TEST(test_at_fora_de_faixa_false);
    exit(UNITY_END());
}
void loop() {}
