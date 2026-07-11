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
    TEST_ASSERT_EQUAL_INT16(-97, out.rssi);
    TEST_ASSERT_EQUAL_INT8(-22, out.snrQuarterDb);
    TEST_ASSERT_EQUAL_UINT16(3, out.rebootCount);
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
    exit(UNITY_END());
}

void loop() {}
