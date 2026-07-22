#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/FlashAuditRing.h"
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

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_append_and_newest_first);
    RUN_TEST(test_wrap_keeps_recent);
    RUN_TEST(test_persists_across_reopen);
    RUN_TEST(test_corrupt_header_formats);
    exit(UNITY_END());
}
void loop() {}
