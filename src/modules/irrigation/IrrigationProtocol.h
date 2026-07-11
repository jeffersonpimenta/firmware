#pragma once
#include <stddef.h>
#include <stdint.h>

namespace IrrigationProto
{

constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_LEN = 8;
constexpr size_t MAX_PAYLOAD = 200;
constexpr uint32_t MAX_OPEN_SECONDS = 120 * 60; // teto absoluto compilado (spec §4.2)

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

// Encoders devolvem bytes totais gravados (header + corpo), 0 se buffer pequeno.
size_t encodeCmdValvula(uint8_t *buf, size_t len, uint32_t seq, const CmdValvula &m);
size_t encodeCmdGpo(uint8_t *buf, size_t len, uint32_t seq, const CmdGpo &m);
size_t encodeAck(uint8_t *buf, size_t len, uint32_t seq, const Ack &m);
size_t encodeHeartbeat(uint8_t *buf, size_t len, uint32_t seq, const Heartbeat &m);

// decodeHeader primeiro; depois o decode do corpo conforme header.type.
bool decodeHeader(const uint8_t *buf, size_t len, Header &out);
bool decodeCmdValvula(const uint8_t *buf, size_t len, CmdValvula &out);
bool decodeCmdGpo(const uint8_t *buf, size_t len, CmdGpo &out);
bool decodeAck(const uint8_t *buf, size_t len, Ack &out);
bool decodeHeartbeat(const uint8_t *buf, size_t len, Heartbeat &out);

} // namespace IrrigationProto
