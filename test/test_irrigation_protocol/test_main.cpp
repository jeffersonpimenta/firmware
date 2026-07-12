#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include <string.h>
#include <unity.h>

using namespace IrrigationProto;

void setUp(void) {}
void tearDown(void) {}

static void test_cmdValvula_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    CmdValvula in = {2, 1, 1800};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 42, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(VERSION, h.version);
    TEST_ASSERT_EQUAL_UINT8(MSG_CMD_VALVULA, h.type);
    TEST_ASSERT_EQUAL_UINT32(42, h.seq);

    CmdValvula out;
    TEST_ASSERT_TRUE(decodeCmdValvula(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(2, out.valveId);
    TEST_ASSERT_EQUAL_UINT8(1, out.action);
    TEST_ASSERT_EQUAL_UINT16(1800, out.durationS);
}

static void test_ack_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    Ack in = {42, ACK_NACK, REASON_BATTERY_LOW, 0b0101, 0, 1215, 17};
    size_t n = encodeAck(buf, sizeof(buf), 7, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Ack out;
    TEST_ASSERT_TRUE(decodeAck(buf, n, out));
    TEST_ASSERT_EQUAL_UINT32(42, out.ackedSeq);
    TEST_ASSERT_EQUAL_UINT8(ACK_NACK, out.status);
    TEST_ASSERT_EQUAL_UINT8(REASON_BATTERY_LOW, out.reason);
    TEST_ASSERT_EQUAL_UINT8(0b0101, out.valveStates);
    TEST_ASSERT_EQUAL_UINT8(0, out.gpoStates);
    TEST_ASSERT_EQUAL_UINT16(1215, out.vbatCentiV);
    TEST_ASSERT_EQUAL_UINT32(17, out.configEpoch);
}

static void test_heartbeat_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    Heartbeat in = {};
    in.valveStates = 0b10;
    in.vbatCentiV = 1250;
    in.vpanelCentiV = 1810;
    in.rssi = -97;
    in.snrQuarterDb = -22; // -5,5 dB em quartos de dB
    in.rebootCount = 3;
    in.rebootCause = 2;
    in.flags = HB_FLAG_TAMPER;
    in.configEpoch = 9;
    size_t n = encodeHeartbeat(buf, sizeof(buf), 100, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Heartbeat out;
    TEST_ASSERT_TRUE(decodeHeartbeat(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(0b10, out.valveStates);
    TEST_ASSERT_EQUAL_UINT16(1250, out.vbatCentiV);
    TEST_ASSERT_EQUAL_UINT16(1810, out.vpanelCentiV);
    TEST_ASSERT_EQUAL_INT16(-97, out.rssi);
    TEST_ASSERT_EQUAL_INT8(-22, out.snrQuarterDb);
    TEST_ASSERT_EQUAL_UINT16(3, out.rebootCount);
    TEST_ASSERT_EQUAL_UINT8(2, out.rebootCause);
    TEST_ASSERT_EQUAL_UINT8(HB_FLAG_TAMPER, out.flags);
    TEST_ASSERT_EQUAL_UINT32(9, out.configEpoch);
}

static void test_truncatedBuffer_rejected()
{
    uint8_t buf[MAX_PAYLOAD];
    CmdValvula in = {0, 1, 60};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 1, in);

    Header h;
    TEST_ASSERT_FALSE(decodeHeader(buf, HEADER_LEN - 1, h));
    CmdValvula out;
    TEST_ASSERT_FALSE(decodeCmdValvula(buf, n - 1, out));
}

static void test_encodeBufferTooSmall_returnsZero()
{
    uint8_t buf[4];
    CmdValvula in = {0, 1, 60};
    TEST_ASSERT_EQUAL_UINT(0, encodeCmdValvula(buf, sizeof(buf), 1, in));
}

static void test_cmdGpo_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    CmdGpo in = {1, 1, 0}; // duração 0 = biestável
    size_t n = encodeCmdGpo(buf, sizeof(buf), 5, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    CmdGpo out;
    TEST_ASSERT_TRUE(decodeCmdGpo(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(1, out.gpoId);
    TEST_ASSERT_EQUAL_UINT16(0, out.durationS);
}

static void test_crc32_referenceVector()
{
    const uint8_t v[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32(v, 9));
    TEST_ASSERT_EQUAL_HEX32(0x00000000, crc32(v, 0));
}

static void test_setConfig_roundTrip()
{
    uint8_t blob[100];
    for (int i = 0; i < 100; i++)
        blob[i] = (uint8_t)i;

    uint8_t buf[MAX_PAYLOAD];
    SetConfig in = {};
    in.epoch = 18;
    in.crc = crc32(blob, 100);
    in.totalLen = 100;
    in.fragIndex = 1;
    in.fragCount = 2;
    in.fragLen = 60;
    in.frag = blob + 40;
    size_t n = encodeSetConfig(buf, sizeof(buf), 9, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_SET_CONFIG, h.type);
    TEST_ASSERT_EQUAL_UINT32(9, h.seq);

    SetConfig out;
    TEST_ASSERT_TRUE(decodeSetConfig(buf, n, out));
    TEST_ASSERT_EQUAL_UINT32(18, out.epoch);
    TEST_ASSERT_EQUAL_HEX32(in.crc, out.crc);
    TEST_ASSERT_EQUAL_UINT16(100, out.totalLen);
    TEST_ASSERT_EQUAL_UINT8(1, out.fragIndex);
    TEST_ASSERT_EQUAL_UINT8(2, out.fragCount);
    TEST_ASSERT_EQUAL_UINT8(60, out.fragLen);
    TEST_ASSERT_EQUAL_MEMORY(blob + 40, out.frag, 60);
}

static void test_setConfig_fragTooBig_rejected()
{
    uint8_t blob[FRAG_DATA_MAX + 1] = {0};
    uint8_t buf[MAX_PAYLOAD];
    SetConfig in = {};
    in.totalLen = sizeof(blob);
    in.fragCount = 1;
    in.fragLen = FRAG_DATA_MAX + 1;
    in.frag = blob;
    TEST_ASSERT_EQUAL_UINT(0, encodeSetConfig(buf, sizeof(buf), 1, in));
}

static void test_setConfig_truncated_rejected()
{
    uint8_t blob[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint8_t buf[MAX_PAYLOAD];
    SetConfig in = {};
    in.totalLen = 10;
    in.fragCount = 1;
    in.fragLen = 10;
    in.frag = blob;
    size_t n = encodeSetConfig(buf, sizeof(buf), 1, in);
    SetConfig out;
    TEST_ASSERT_FALSE(decodeSetConfig(buf, n - 1, out)); // corta 1 byte do frag
}

static void test_getConfig_headerOnly()
{
    uint8_t buf[MAX_PAYLOAD];
    size_t n = encodeGetConfig(buf, sizeof(buf), 3);
    TEST_ASSERT_EQUAL_UINT(HEADER_LEN, n);
    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_GET_CONFIG, h.type);
    TEST_ASSERT_EQUAL_UINT32(3, h.seq);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_cmdValvula_roundTrip);
    RUN_TEST(test_ack_roundTrip);
    RUN_TEST(test_heartbeat_roundTrip);
    RUN_TEST(test_truncatedBuffer_rejected);
    RUN_TEST(test_encodeBufferTooSmall_returnsZero);
    RUN_TEST(test_cmdGpo_roundTrip);
    RUN_TEST(test_crc32_referenceVector);
    RUN_TEST(test_setConfig_roundTrip);
    RUN_TEST(test_setConfig_fragTooBig_rejected);
    RUN_TEST(test_setConfig_truncated_rejected);
    RUN_TEST(test_getConfig_headerOnly);
    exit(UNITY_END());
}

void loop() {}
