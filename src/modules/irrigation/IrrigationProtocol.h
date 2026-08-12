#pragma once
#include <stddef.h>
#include <stdint.h>

namespace IrrigationProto
{

constexpr uint8_t VERSION = 1;
constexpr uint16_t FLAG_FROM_SERVICE = 0x0001; // Header.flags: remetente é nó de serviço (§11.5)
constexpr uint16_t APP_FW_VERSION = 0x0800;    // geração do firmware de irrigação (Fase 8a); p/ planejamento de OTA (§3.3/§11.4)
constexpr size_t HEADER_LEN = 8;
constexpr size_t MAX_PAYLOAD = 200;
constexpr uint32_t MAX_OPEN_SECONDS = 120 * 60; // teto absoluto compilado (spec §4.2)
constexpr uint8_t FRAG_DATA_MAX = 160;
constexpr uint8_t HB_MAX_SENSORS = 4;

// true = remetente autorizado a comandar (§4.2, §11.5). Puro: sem estado.
inline bool senderAuthorizedBy(uint16_t flags, uint32_t from, uint32_t boundGateway)
{
    if (flags & FLAG_FROM_SERVICE)
        return true;                                  // marca de serviço dispensa vínculo (§11.5)
    return boundGateway == 0 || from == boundGateway; // posse-da-PSK / vínculo (§4.2)
}

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
    MSG_CMD_MAINT = 13, // janela de manutenção do tamper (panel → gateway → estação)
    MSG_REMOTE_TRIGGER = 14, // entrada digital remota (painel → gateway)
    MSG_REMOTE_LED = 15,     // estado de LEDs remoto (gateway → painel)
};

enum AckStatus : uint8_t { ACK_OK = 0, ACK_NACK = 1 };

// Ação de comando de saída (CmdValvula/CmdGpo): 0 = fechar, 1 = abrir.
// 2 = TOGGLE: o nó alvo inverte a própria saída (modo remoto P2P fallback).
constexpr uint8_t ACTION_TOGGLE = 2;

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

struct CmdMaint {
    uint16_t durationMin; // 0 = fechar janela agora
};

struct RemoteTrigger {
    uint8_t inputIdx; // 0..3
};

struct RemoteLed {
    uint8_t ledStates; // bit s = slot de LED s ligado
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

struct SensorReading {
    uint8_t id;
    uint8_t tipo;       // 0 digital, 1 analógico
    int16_t valueCenti; // digital: 0/100
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
    // Bloco de sensores opcional (trailing); ausente em payloads de firmware anterior
    uint8_t sensorCount = 0;
    SensorReading sensors[HB_MAX_SENSORS] = {};
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
    EV_FACTORY_RESET = 5,
    EV_TAMPER = 6, // arg: 1 = abriu, 0 = fechou
};

struct Evento {
    uint8_t code;
    uint32_t arg;
};

struct ResyncSeq {
    uint8_t kind;     // 0 = REQUEST (CTRL→EST), 1 = REPLY (EST→CTRL)
    uint32_t lastSeq; // válido só no REPLY: last_seq registrado p/ o remetente do REQUEST
};

struct PingSurvey {
    uint8_t kind;         // 0 = PROBE (sonda, req), 1 = REPLY
    uint8_t role;         // IrrigationRole (válido no REPLY)
    uint32_t configEpoch; // válido no REPLY
    uint16_t vbatCentiV;  // válido no REPLY
    uint16_t fwVersion;   // válido no REPLY (APP_FW_VERSION do respondente)
    int32_t latE7;        // válido no REPLY (0 se sem coordenada)
    int32_t lonE7;
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
size_t encodeCmdMaint(uint8_t *buf, size_t len, uint32_t seq, const CmdMaint &m);
size_t encodeRemoteTrigger(uint8_t *buf, size_t len, uint32_t seq, const RemoteTrigger &m);
size_t encodeRemoteLed(uint8_t *buf, size_t len, uint32_t seq, const RemoteLed &m);
size_t encodeResyncSeq(uint8_t *buf, size_t len, uint32_t seq, const ResyncSeq &m);
size_t encodePingSurvey(uint8_t *buf, size_t len, uint32_t seq, const PingSurvey &m);

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
bool decodeCmdMaint(const uint8_t *buf, size_t len, CmdMaint &out);
bool decodeRemoteTrigger(const uint8_t *buf, size_t len, RemoteTrigger &out);
bool decodeRemoteLed(const uint8_t *buf, size_t len, RemoteLed &out);
bool decodeResyncSeq(const uint8_t *buf, size_t len, ResyncSeq &out);
bool decodePingSurvey(const uint8_t *buf, size_t len, PingSurvey &out);

// Marca um pacote já codificado como originado pelo nó de serviço (§11.5); flags no offset 2 (LE).
void setServiceFlag(uint8_t *buf, size_t len);

uint32_t crc32(const uint8_t *data, size_t len);

} // namespace IrrigationProto
