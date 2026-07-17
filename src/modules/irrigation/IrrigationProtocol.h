#pragma once
#include <stddef.h>
#include <stdint.h>

namespace IrrigationProto
{

constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_LEN = 8;
constexpr size_t MAX_PAYLOAD = 200;
constexpr uint32_t MAX_OPEN_SECONDS = 120 * 60; // teto absoluto compilado (spec §4.2)
constexpr uint8_t FRAG_DATA_MAX = 160;

enum MsgType : uint8_t {
    MSG_CMD_VALVULA = 1,
    MSG_CMD_GPO = 2,
    MSG_ACK = 3,
    MSG_HEARTBEAT = 4,
    MSG_SET_CONFIG = 5,
    MSG_GET_CONFIG = 6,
    MSG_EVENTO = 7,
    MSG_PAIR_ANNOUNCE = 8,
    MSG_PAIR_GRANT = 9,
    MSG_PING_SURVEY = 10,
    MSG_RESYNC_SEQ = 11,
    MSG_REMOTE_CMD = 12,
};

enum AckStatus : uint8_t { ACK_OK = 0, ACK_NACK = 1 };

enum NackReason : uint8_t {
    REASON_NONE = 0,
    REASON_BATTERY_LOW = 1,
    REASON_RATE_LIMIT = 2,
    REASON_INVALID_ID = 3,
    REASON_UNAUTHORIZED = 4,
    REASON_BAD_VERSION = 5,
    REASON_BAD_PAYLOAD = 6,
    REASON_SAFE_MODE = 7,
    REASON_BAD_CRC = 8,
    REASON_CONFIG_TOO_BIG = 9,
    REASON_FRAG_INVALID = 10,
    REASON_COMMIT_FAIL = 11,
};

enum HbFlags : uint8_t {
    HB_FLAG_TAMPER = 1 << 0,
    HB_FLAG_SAFE_MODE = 1 << 1,
    HB_FLAG_HIBERNATION = 1 << 2,
};

struct Header {
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t seq;
};

struct CmdValvula {
    uint8_t valveId;
    uint8_t action; // 0 = fechar, 1 = abrir
    uint16_t durationS;
};

struct CmdGpo {
    uint8_t gpoId;
    uint8_t action;
    uint16_t durationS; // 0 = biestável
};

struct RemoteCmd {
    uint8_t zoneId;    // id de zona no gateway (1..255)
    uint8_t action;    // 0 = fechar, 1 = abrir
    uint16_t durationS;
};

struct Ack {
    uint32_t ackedSeq;
    uint8_t status; // AckStatus
    uint8_t reason; // NackReason
    uint8_t valveStates;
    uint8_t gpoStates;
    uint16_t vbatCentiV;
    uint32_t configEpoch;
};

struct Heartbeat {
    uint8_t valveStates;
    uint8_t gpoStates;
    uint16_t vbatCentiV;
    uint16_t vpanelCentiV;
    int16_t rssi;
    int8_t snrQuarterDb;
    uint16_t rebootCount;
    uint8_t rebootCause;
    uint8_t flags; // HbFlags
    uint32_t configEpoch;
};

struct SetConfig {
    uint32_t epoch;
    uint32_t crc;      // crc32 do blob completo
    uint16_t totalLen; // bytes do blob completo
    uint8_t fragIndex;
    uint8_t fragCount;
    uint8_t fragLen;
    const uint8_t *frag; // decode: aponta dentro do buffer de entrada
};

struct PairAnnounce {
    uint8_t protoVersion;
    uint8_t nameLen;
    char name[16];
};

struct PairGrant {
    uint8_t psk[32];
    uint8_t nameLen;
    char channelName[12];
    uint32_t gatewayId;
};

enum EventCode : uint8_t {
    EV_MANUAL_OPEN = 1,
    EV_MANUAL_CLOSE = 2,
    EV_TEST_PULSE = 3,
    EV_PAIRED = 4,
    EV_FACTORY_RESET = 5
};

struct Evento {
    uint8_t code;
    uint32_t arg;
};

// Encoders devolvem bytes totais gravados (header + corpo), 0 se buffer pequeno.
size_t encodeCmdValvula(uint8_t *buf, size_t len, uint32_t seq, const CmdValvula &m);
size_t encodeCmdGpo(uint8_t *buf, size_t len, uint32_t seq, const CmdGpo &m);
size_t encodeAck(uint8_t *buf, size_t len, uint32_t seq, const Ack &m);
size_t encodeHeartbeat(uint8_t *buf, size_t len, uint32_t seq, const Heartbeat &m);
size_t encodeSetConfig(uint8_t *buf, size_t len, uint32_t seq, const SetConfig &m);
size_t encodeGetConfig(uint8_t *buf, size_t len, uint32_t seq);
size_t encodePairAnnounce(uint8_t *buf, size_t len, uint32_t seq, const PairAnnounce &m);
size_t encodePairGrant(uint8_t *buf, size_t len, uint32_t seq, const PairGrant &m);
size_t encodeEvento(uint8_t *buf, size_t len, uint32_t seq, const Evento &m);
size_t encodeRemoteCmd(uint8_t *buf, size_t len, uint32_t seq, const RemoteCmd &m);

// decodeHeader primeiro; depois o decode do corpo conforme header.type.
bool decodeHeader(const uint8_t *buf, size_t len, Header &out);
bool decodeCmdValvula(const uint8_t *buf, size_t len, CmdValvula &out);
bool decodeCmdGpo(const uint8_t *buf, size_t len, CmdGpo &out);
bool decodeAck(const uint8_t *buf, size_t len, Ack &out);
bool decodeHeartbeat(const uint8_t *buf, size_t len, Heartbeat &out);
bool decodeSetConfig(const uint8_t *buf, size_t len, SetConfig &out);
bool decodePairAnnounce(const uint8_t *buf, size_t len, PairAnnounce &out);
bool decodePairGrant(const uint8_t *buf, size_t len, PairGrant &out);
bool decodeEvento(const uint8_t *buf, size_t len, Evento &out);
bool decodeRemoteCmd(const uint8_t *buf, size_t len, RemoteCmd &out);

uint32_t crc32(const uint8_t *data, size_t len);

} // namespace IrrigationProto
