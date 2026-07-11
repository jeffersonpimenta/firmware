#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/FragmentReassembler.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include <string.h>
#include <unity.h>

using Add = FragmentReassembler::Add;

void setUp(void) {}
void tearDown(void) {}

static uint8_t blob[300];
static uint32_t blobCrc;

static void makeBlob()
{
    for (int i = 0; i < 300; i++)
        blob[i] = (uint8_t)(i * 7);
    blobCrc = IrrigationProto::crc32(blob, 300);
}

// 300 B em 2 fragmentos de 160 + 140.
static Add feed(FragmentReassembler &r, uint8_t idx, uint32_t nowMs, uint32_t sender = 1, uint32_t epoch = 5)
{
    uint16_t off = idx * 160;
    uint8_t len = (idx == 0) ? 160 : 140;
    return r.add(sender, epoch, blobCrc, 300, idx, 2, blob + off, len, nowMs);
}

static void test_inOrder_completesWithMatchingBlob()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1000));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 1, 2000));
    TEST_ASSERT_EQUAL_UINT16(300, r.blobLen());
    TEST_ASSERT_EQUAL_UINT32(5, r.epoch());
    TEST_ASSERT_EQUAL_MEMORY(blob, r.blob(), 300);
}

static void test_outOfOrder_completes()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 1, 1000));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 0, 2000));
    TEST_ASSERT_EQUAL_MEMORY(blob, r.blob(), 300);
}

static void test_duplicateFragment_idempotent()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1000));
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1500));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 1, 2000));
}

static void test_badCrc_rejectedAndReset()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, r.add(1, 5, blobCrc ^ 1, 300, 0, 2, blob, 160, 1000));
    TEST_ASSERT_EQUAL(Add::BAD_CRC, r.add(1, 5, blobCrc ^ 1, 300, 1, 2, blob + 160, 140, 2000));
    // após BAD_CRC a transferência morre; recomeço do zero funciona
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 3000));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 1, 4000));
}

static void test_newKeyDropsOldTransfer()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1000, /*sender=*/1));
    // outro remetente começa: transferência antiga descartada
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1500, /*sender=*/2));
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 1, 2000, /*sender=*/2));
}

static void test_timeoutDropsStaleTransfer()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 0, 1000));
    // 31 s depois só chegou o frag 1: é tratado como transferência nova (incompleta)
    TEST_ASSERT_EQUAL(Add::ACCEPTED, feed(r, 1, 1000 + 31000));
    // e o frag 0 completa
    TEST_ASSERT_EQUAL(Add::COMPLETE, feed(r, 0, 1000 + 32000));
}

static void test_limitsRejected()
{
    makeBlob();
    FragmentReassembler r;
    TEST_ASSERT_EQUAL(Add::TOO_BIG, r.add(1, 5, blobCrc, 513, 0, 4, blob, 160, 1000));
    TEST_ASSERT_EQUAL(Add::TOO_BIG, r.add(1, 5, blobCrc, 300, 0, 17, blob, 20, 1000));
    TEST_ASSERT_EQUAL(Add::INVALID, r.add(1, 5, blobCrc, 300, 2, 2, blob, 100, 1000)); // idx >= count
    TEST_ASSERT_EQUAL(Add::INVALID, r.add(1, 5, blobCrc, 300, 0, 2, blob, 0, 1000));   // fragLen 0
    // frag 1 de 2 com totalLen=200: off=160, 160+100=260 > 200 → INVALID (geometria)
    TEST_ASSERT_EQUAL(Add::INVALID, r.add(1, 5, blobCrc, 200, 1, 2, blob, 100, 1000));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_inOrder_completesWithMatchingBlob);
    RUN_TEST(test_outOfOrder_completes);
    RUN_TEST(test_duplicateFragment_idempotent);
    RUN_TEST(test_badCrc_rejectedAndReset);
    RUN_TEST(test_newKeyDropsOldTransfer);
    RUN_TEST(test_timeoutDropsStaleTransfer);
    RUN_TEST(test_limitsRejected);
    exit(UNITY_END());
}

void loop() {}
