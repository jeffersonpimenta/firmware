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

static void test_pairAnnounce_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    PairAnnounce in = {};
    in.protoVersion = VERSION;
    in.nameLen = 5;
    memcpy(in.name, "Pasto", 5);
    size_t n = encodePairAnnounce(buf, sizeof(buf), 2, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_PAIR_ANNOUNCE, h.type);
    PairAnnounce out;
    TEST_ASSERT_TRUE(decodePairAnnounce(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(VERSION, out.protoVersion);
    TEST_ASSERT_EQUAL_UINT8(5, out.nameLen);
    TEST_ASSERT_EQUAL_STRING("Pasto", out.name);
}

static void test_pairGrant_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    PairGrant in = {};
    for (int i = 0; i < 32; i++)
        in.psk[i] = (uint8_t)(i * 3);
    in.nameLen = 7;
    memcpy(in.channelName, "bvirrig", 7);
    in.gatewayId = 0xa1b2c3d4;
    size_t n = encodePairGrant(buf, sizeof(buf), 3, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    PairGrant out;
    TEST_ASSERT_TRUE(decodePairGrant(buf, n, out));
    TEST_ASSERT_EQUAL_MEMORY(in.psk, out.psk, 32);
    TEST_ASSERT_EQUAL_STRING("bvirrig", out.channelName);
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, out.gatewayId);
}

static void test_pairGrant_nameTooLong_rejected()
{
    uint8_t buf[MAX_PAYLOAD];
    PairGrant in = {};
    in.nameLen = 12; // máx 11
    TEST_ASSERT_EQUAL_UINT(0, encodePairGrant(buf, sizeof(buf), 1, in));
}

static void test_evento_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    Evento in = {EV_MANUAL_OPEN, 1200};
    size_t n = encodeEvento(buf, sizeof(buf), 4, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    Evento out;
    TEST_ASSERT_TRUE(decodeEvento(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(EV_MANUAL_OPEN, out.code);
    TEST_ASSERT_EQUAL_UINT32(1200, out.arg);
}

static void test_pairGrant_truncated_rejected()
{
    uint8_t buf[MAX_PAYLOAD];
    PairGrant in = {};
    in.nameLen = 4;
    memcpy(in.channelName, "abcd", 4);
    in.gatewayId = 1;
    size_t n = encodePairGrant(buf, sizeof(buf), 1, in);
    PairGrant out;
    TEST_ASSERT_FALSE(decodePairGrant(buf, n - 1, out));
}

static void test_remoteCmd_roundtrip()
{
    using namespace IrrigationProto;
    uint8_t buf[32];
    RemoteCmd m = {};
    m.zoneId = 7;
    m.action = 1;
    m.durationS = 600;
    size_t n = encodeRemoteCmd(buf, sizeof(buf), 0x11223344, m);
    TEST_ASSERT_GREATER_THAN(0, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_REMOTE_CMD, h.type);
    TEST_ASSERT_EQUAL_UINT32(0x11223344, h.seq);

    RemoteCmd out = {};
    TEST_ASSERT_TRUE(decodeRemoteCmd(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(7, out.zoneId);
    TEST_ASSERT_EQUAL_UINT8(1, out.action);
    TEST_ASSERT_EQUAL_UINT16(600, out.durationS);
}

static void test_remoteCmd_shortBufferFails()
{
    using namespace IrrigationProto;
    uint8_t buf[4]; // menor que o header
    RemoteCmd m = {};
    TEST_ASSERT_EQUAL_UINT(0, encodeRemoteCmd(buf, sizeof(buf), 1, m));
}

static void test_heartbeat_sensor_block_roundtrip()
{
    Heartbeat hb = {};
    hb.valveStates = 0x3;
    hb.configEpoch = 7;
    hb.sensorCount = 2;
    hb.sensors[0] = {0, 1, 152};  // analógico 1,52
    hb.sensors[1] = {1, 0, 100};  // digital ativo
    uint8_t buf[64];
    size_t n = encodeHeartbeat(buf, sizeof(buf), 1, hb);
    TEST_ASSERT_TRUE(n > 0);
    Heartbeat out;
    TEST_ASSERT_TRUE(decodeHeartbeat(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(2, out.sensorCount);
    TEST_ASSERT_EQUAL_INT16(152, out.sensors[0].valueCenti);
    TEST_ASSERT_EQUAL_UINT8(0, out.sensors[1].tipo);
}

static void test_heartbeat_sensor_block_truncated_rejected()
{
    Heartbeat hb = {};
    hb.sensorCount = 2;
    hb.sensors[0] = {0, 1, 152};
    hb.sensors[1] = {1, 0, 100};
    uint8_t buf[64];
    size_t n = encodeHeartbeat(buf, sizeof(buf), 1, hb);
    TEST_ASSERT_TRUE(n > 0);
    Heartbeat out;
    TEST_ASSERT_FALSE(decodeHeartbeat(buf, n - 1, out));
}

static void test_heartbeat_legacy_payload_decodes_zero_sensors()
{
    Heartbeat hb = {};
    hb.sensorCount = 0;
    uint8_t buf[64];
    size_t n = encodeHeartbeat(buf, sizeof(buf), 1, hb);
    // count=0 emite só o byte de contagem; truncar também o byte simula payload v. anterior
    Heartbeat out;
    TEST_ASSERT_TRUE(decodeHeartbeat(buf, n - 1, out));
    TEST_ASSERT_EQUAL_UINT8(0, out.sensorCount);
}

static void test_heartbeat_sensor_count_overflow_rejected()
{
    Heartbeat hb = {};
    uint8_t buf[64];
    size_t n = encodeHeartbeat(buf, sizeof(buf), 1, hb);
    buf[n - 1] = 5; // count > HB_MAX_SENSORS
    Heartbeat out;
    TEST_ASSERT_FALSE(decodeHeartbeat(buf, n, out));
}

static void test_evento_tamper_roundtrip()
{
    uint8_t buf[MAX_PAYLOAD];
    Evento in = {EV_TAMPER, 1}; // 1 = abriu
    size_t n = encodeEvento(buf, sizeof(buf), 10, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);
    Evento out;
    TEST_ASSERT_TRUE(decodeEvento(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(EV_TAMPER, out.code);
    TEST_ASSERT_EQUAL_UINT32(1, out.arg);
}

static void test_cmd_maint_roundtrip()
{
    using namespace IrrigationProto;
    uint8_t buf[64];
    CmdMaint m{15}; // 15 min
    size_t n = encodeCmdMaint(buf, sizeof(buf), 42, m);
    TEST_ASSERT_GREATER_THAN(0, n);
    Header h; TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_CMD_MAINT, h.type);
    TEST_ASSERT_EQUAL_UINT32(42, h.seq);
    CmdMaint out; TEST_ASSERT_TRUE(decodeCmdMaint(buf, n, out));
    TEST_ASSERT_EQUAL_UINT16(15, out.durationMin);
}

static void test_serviceFlag_setAndRead()
{
    uint8_t buf[MAX_PAYLOAD];
    CmdValvula in = {0, 1, 600};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 5, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h0;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h0));
    TEST_ASSERT_EQUAL_UINT16(0, h0.flags & FLAG_FROM_SERVICE); // default: sem marca

    setServiceFlag(buf, n);

    Header h1;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h1));
    TEST_ASSERT_TRUE((h1.flags & FLAG_FROM_SERVICE) != 0); // marcado
    // corpo intacto após marcar
    CmdValvula out;
    TEST_ASSERT_TRUE(decodeCmdValvula(buf, n, out));
    TEST_ASSERT_EQUAL_UINT16(600, out.durationS);
}

// C1: comando direto de fallback P2P parte da botoeira (não é o gateway). Um alvo pareado
// (boundGateway != 0) só autoriza o gateway OU um remetente marcado como serviço. Sem a marca
// o peer é rejeitado; com a marca (o que a estação carimba) é aceito.
static void test_p2p_toggle_peer_needs_service_flag_to_authorize()
{
    const uint32_t gateway = 0xAAAAAAAA;
    const uint32_t botoeira = 0xBBBBBBBB; // peer, não é o gateway
    uint8_t buf[MAX_PAYLOAD];
    CmdValvula in = {1, ACTION_TOGGLE, 0};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 7, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h0;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h0));
    // Sem marca: alvo pareado rejeita um peer que não é o gateway.
    TEST_ASSERT_FALSE(senderAuthorizedBy(h0.flags, botoeira, gateway));

    setServiceFlag(buf, n);
    Header h1;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h1));
    // Com marca: autorizado (mecanismo que a estação usa no fallback direto).
    TEST_ASSERT_TRUE(senderAuthorizedBy(h1.flags, botoeira, gateway));
}

static void test_resyncSeq_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    ResyncSeq in = {1, 4242}; // REPLY carregando lastSeq
    size_t n = encodeResyncSeq(buf, sizeof(buf), 7, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_RESYNC_SEQ, h.type);

    ResyncSeq out;
    TEST_ASSERT_TRUE(decodeResyncSeq(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(1, out.kind);
    TEST_ASSERT_EQUAL_UINT32(4242, out.lastSeq);
}

static void test_pingSurvey_roundTrip()
{
    uint8_t buf[MAX_PAYLOAD];
    PingSurvey in = {};
    in.kind = 1; // REPLY
    in.role = 1; // GATEWAY
    in.configEpoch = 17;
    in.vbatCentiV = 1250;
    in.fwVersion = APP_FW_VERSION;
    in.latE7 = -221000000;
    in.lonE7 = -476000000;
    size_t n = encodePingSurvey(buf, sizeof(buf), 9, in);
    TEST_ASSERT_GREATER_THAN(HEADER_LEN, n);

    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_PING_SURVEY, h.type);

    PingSurvey out;
    TEST_ASSERT_TRUE(decodePingSurvey(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(1, out.kind);
    TEST_ASSERT_EQUAL_UINT8(1, out.role);
    TEST_ASSERT_EQUAL_UINT32(17, out.configEpoch);
    TEST_ASSERT_EQUAL_UINT16(1250, out.vbatCentiV);
    TEST_ASSERT_EQUAL_UINT16(APP_FW_VERSION, out.fwVersion);
    TEST_ASSERT_EQUAL_INT32(-221000000, out.latE7);
    TEST_ASSERT_EQUAL_INT32(-476000000, out.lonE7);
}

static void test_resyncPing_truncated_rejected()
{
    uint8_t buf[MAX_PAYLOAD];
    ResyncSeq r = {0, 0};
    size_t nr = encodeResyncSeq(buf, sizeof(buf), 1, r);
    ResyncSeq ro;
    TEST_ASSERT_FALSE(decodeResyncSeq(buf, nr - 1, ro)); // corta 1 byte do lastSeq

    PingSurvey p = {};
    p.kind = 1;
    size_t np = encodePingSurvey(buf, sizeof(buf), 1, p);
    PingSurvey po;
    TEST_ASSERT_FALSE(decodePingSurvey(buf, np - 1, po)); // corta 1 byte do lonE7
}

static void test_remoteTrigger_roundTrip()
{
    uint8_t buf[32];
    IrrigationProto::RemoteTrigger m{3};
    size_t n = IrrigationProto::encodeRemoteTrigger(buf, sizeof(buf), 0x1122, m);
    TEST_ASSERT_TRUE(n > 0);
    IrrigationProto::Header h;
    TEST_ASSERT_TRUE(IrrigationProto::decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(IrrigationProto::MSG_REMOTE_TRIGGER, h.type);
    TEST_ASSERT_EQUAL_UINT32(0x1122, h.seq);
    IrrigationProto::RemoteTrigger out{};
    TEST_ASSERT_TRUE(IrrigationProto::decodeRemoteTrigger(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(3, out.inputIdx);
    // buffer curto → 0
    TEST_ASSERT_EQUAL_UINT(0, IrrigationProto::encodeRemoteTrigger(buf, 2, 1, m));
}

static void test_remoteLed_roundTrip()
{
    uint8_t buf[32];
    IrrigationProto::RemoteLed m{0b10};
    size_t n = IrrigationProto::encodeRemoteLed(buf, sizeof(buf), 7, m);
    TEST_ASSERT_TRUE(n > 0);
    IrrigationProto::RemoteLed out{};
    TEST_ASSERT_TRUE(IrrigationProto::decodeRemoteLed(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(0b10, out.ledStates);
}

void test_cmdValvula_toggle_action_roundtrip()
{
    using namespace IrrigationProto;
    uint8_t buf[32];
    CmdValvula in{3, ACTION_TOGGLE, 0};
    size_t n = encodeCmdValvula(buf, sizeof(buf), 42, in);
    TEST_ASSERT_TRUE(n > 0);
    Header h;
    TEST_ASSERT_TRUE(decodeHeader(buf, n, h));
    TEST_ASSERT_EQUAL_UINT8(MSG_CMD_VALVULA, h.type);
    CmdValvula out;
    TEST_ASSERT_TRUE(decodeCmdValvula(buf, n, out));
    TEST_ASSERT_EQUAL_UINT8(3, out.valveId);
    TEST_ASSERT_EQUAL_UINT8(ACTION_TOGGLE, out.action);
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
    RUN_TEST(test_pairAnnounce_roundTrip);
    RUN_TEST(test_pairGrant_roundTrip);
    RUN_TEST(test_pairGrant_nameTooLong_rejected);
    RUN_TEST(test_evento_roundTrip);
    RUN_TEST(test_pairGrant_truncated_rejected);
    RUN_TEST(test_remoteCmd_roundtrip);
    RUN_TEST(test_remoteCmd_shortBufferFails);
    RUN_TEST(test_heartbeat_sensor_block_roundtrip);
    RUN_TEST(test_heartbeat_sensor_block_truncated_rejected);
    RUN_TEST(test_heartbeat_legacy_payload_decodes_zero_sensors);
    RUN_TEST(test_heartbeat_sensor_count_overflow_rejected);
    RUN_TEST(test_evento_tamper_roundtrip);
    RUN_TEST(test_cmd_maint_roundtrip);
    RUN_TEST(test_serviceFlag_setAndRead);
    RUN_TEST(test_resyncSeq_roundTrip);
    RUN_TEST(test_pingSurvey_roundTrip);
    RUN_TEST(test_resyncPing_truncated_rejected);
    RUN_TEST(test_remoteTrigger_roundTrip);
    RUN_TEST(test_remoteLed_roundTrip);
    RUN_TEST(test_cmdValvula_toggle_action_roundtrip);
    RUN_TEST(test_p2p_toggle_peer_needs_service_flag_to_authorize);
    exit(UNITY_END());
}

void loop() {}
