#include "IrrigationProtocol.h"
#include <string.h>

namespace IrrigationProto
{

namespace
{
// Serialização little-endian explícita: independe de endianness/padding do alvo.
struct Writer {
    uint8_t *buf;
    size_t cap;
    size_t pos = 0;
    bool ok = true;
    void u8(uint8_t v)
    {
        if (pos + 1 > cap) {
            ok = false;
            return;
        }
        buf[pos++] = v;
    }
    void u16(uint16_t v)
    {
        u8(v & 0xff);
        u8(v >> 8);
    }
    void u32(uint32_t v)
    {
        u16(v & 0xffff);
        u16(v >> 16);
    }
    void i8(int8_t v) { u8((uint8_t)v); }
    void i16(int16_t v) { u16((uint16_t)v); }
};

struct Reader {
    const uint8_t *buf;
    size_t len;
    size_t pos = 0;
    bool ok = true;
    uint8_t u8()
    {
        if (pos + 1 > len) {
            ok = false;
            return 0;
        }
        return buf[pos++];
    }
    uint16_t u16()
    {
        uint16_t lo = u8();
        return lo | ((uint16_t)u8() << 8);
    }
    uint32_t u32()
    {
        uint32_t lo = u16();
        return lo | ((uint32_t)u16() << 16);
    }
    int8_t i8() { return (int8_t)u8(); }
    int16_t i16() { return (int16_t)u16(); }
};

void writeHeader(Writer &w, uint8_t type, uint32_t seq)
{
    w.u8(VERSION);
    w.u8(type);
    w.u16(0); // flags (reservado; SERVICE_MAGIC na fase 8)
    w.u32(seq);
}

Reader bodyReader(const uint8_t *buf, size_t len)
{
    Reader r{buf, len};
    r.pos = HEADER_LEN;
    if (len < HEADER_LEN)
        r.ok = false;
    return r;
}
} // namespace

size_t encodeCmdValvula(uint8_t *buf, size_t len, uint32_t seq, const CmdValvula &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_CMD_VALVULA, seq);
    w.u8(m.valveId);
    w.u8(m.action);
    w.u16(m.durationS);
    return w.ok ? w.pos : 0;
}

size_t encodeCmdGpo(uint8_t *buf, size_t len, uint32_t seq, const CmdGpo &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_CMD_GPO, seq);
    w.u8(m.gpoId);
    w.u8(m.action);
    w.u16(m.durationS);
    return w.ok ? w.pos : 0;
}

size_t encodeAck(uint8_t *buf, size_t len, uint32_t seq, const Ack &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_ACK, seq);
    w.u32(m.ackedSeq);
    w.u8(m.status);
    w.u8(m.reason);
    w.u8(m.valveStates);
    w.u8(m.gpoStates);
    w.u16(m.vbatCentiV);
    w.u32(m.configEpoch);
    return w.ok ? w.pos : 0;
}

size_t encodeHeartbeat(uint8_t *buf, size_t len, uint32_t seq, const Heartbeat &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_HEARTBEAT, seq);
    w.u8(m.valveStates);
    w.u8(m.gpoStates);
    w.u16(m.vbatCentiV);
    w.u16(m.vpanelCentiV);
    w.i16(m.rssi);
    w.i8(m.snrQuarterDb);
    w.u16(m.rebootCount);
    w.u8(m.rebootCause);
    w.u8(m.flags);
    w.u32(m.configEpoch);
    uint8_t cnt = m.sensorCount > HB_MAX_SENSORS ? HB_MAX_SENSORS : m.sensorCount;
    w.u8(cnt);
    for (uint8_t i = 0; i < cnt; i++) {
        w.u8(m.sensors[i].id);
        w.u8(m.sensors[i].tipo);
        w.i16(m.sensors[i].valueCenti);
    }
    return w.ok ? w.pos : 0;
}

bool decodeHeader(const uint8_t *buf, size_t len, Header &out)
{
    Reader r{buf, len};
    out.version = r.u8();
    out.type = r.u8();
    out.flags = r.u16();
    out.seq = r.u32();
    return r.ok;
}

bool decodeCmdValvula(const uint8_t *buf, size_t len, CmdValvula &out)
{
    Reader r = bodyReader(buf, len);
    out.valveId = r.u8();
    out.action = r.u8();
    out.durationS = r.u16();
    return r.ok;
}

bool decodeCmdGpo(const uint8_t *buf, size_t len, CmdGpo &out)
{
    Reader r = bodyReader(buf, len);
    out.gpoId = r.u8();
    out.action = r.u8();
    out.durationS = r.u16();
    return r.ok;
}

bool decodeAck(const uint8_t *buf, size_t len, Ack &out)
{
    Reader r = bodyReader(buf, len);
    out.ackedSeq = r.u32();
    out.status = r.u8();
    out.reason = r.u8();
    out.valveStates = r.u8();
    out.gpoStates = r.u8();
    out.vbatCentiV = r.u16();
    out.configEpoch = r.u32();
    return r.ok;
}

bool decodeHeartbeat(const uint8_t *buf, size_t len, Heartbeat &out)
{
    Reader r = bodyReader(buf, len);
    out.valveStates = r.u8();
    out.gpoStates = r.u8();
    out.vbatCentiV = r.u16();
    out.vpanelCentiV = r.u16();
    out.rssi = r.i16();
    out.snrQuarterDb = r.i8();
    out.rebootCount = r.u16();
    out.rebootCause = r.u8();
    out.flags = r.u8();
    out.configEpoch = r.u32();
    if (!r.ok)
        return false;
    // Bloco de sensores é opcional (payloads de firmware anterior não o têm)
    out.sensorCount = 0;
    if (r.pos >= len)
        return true;
    uint8_t cnt = r.u8();
    if (cnt > HB_MAX_SENSORS)
        return false;
    for (uint8_t i = 0; i < cnt; i++) {
        out.sensors[i].id = r.u8();
        out.sensors[i].tipo = r.u8();
        out.sensors[i].valueCenti = r.i16();
    }
    out.sensorCount = r.ok ? cnt : 0;
    return r.ok;
}

uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

size_t encodeSetConfig(uint8_t *buf, size_t len, uint32_t seq, const SetConfig &m)
{
    if (m.fragLen > FRAG_DATA_MAX)
        return 0;
    Writer w{buf, len};
    writeHeader(w, MSG_SET_CONFIG, seq);
    w.u32(m.epoch);
    w.u32(m.crc);
    w.u16(m.totalLen);
    w.u8(m.fragIndex);
    w.u8(m.fragCount);
    w.u8(m.fragLen);
    for (uint8_t i = 0; w.ok && i < m.fragLen; i++)
        w.u8(m.frag[i]);
    return w.ok ? w.pos : 0;
}

bool decodeSetConfig(const uint8_t *buf, size_t len, SetConfig &out)
{
    Reader r = bodyReader(buf, len);
    out.epoch = r.u32();
    out.crc = r.u32();
    out.totalLen = r.u16();
    out.fragIndex = r.u8();
    out.fragCount = r.u8();
    out.fragLen = r.u8();
    if (!r.ok || out.fragLen > FRAG_DATA_MAX || r.pos + out.fragLen > len)
        return false;
    out.frag = buf + r.pos;
    return true;
}

size_t encodeGetConfig(uint8_t *buf, size_t len, uint32_t seq)
{
    Writer w{buf, len};
    writeHeader(w, MSG_GET_CONFIG, seq);
    return w.ok ? w.pos : 0;
}

size_t encodePairAnnounce(uint8_t *buf, size_t len, uint32_t seq, const PairAnnounce &m)
{
    if (m.nameLen > 15)
        return 0;
    Writer w{buf, len};
    writeHeader(w, MSG_PAIR_ANNOUNCE, seq);
    w.u8(m.protoVersion);
    w.u8(m.nameLen);
    for (uint8_t i = 0; w.ok && i < m.nameLen; i++)
        w.u8((uint8_t)m.name[i]);
    return w.ok ? w.pos : 0;
}

bool decodePairAnnounce(const uint8_t *buf, size_t len, PairAnnounce &out)
{
    Reader r = bodyReader(buf, len);
    out.protoVersion = r.u8();
    out.nameLen = r.u8();
    if (!r.ok || out.nameLen > 15 || r.pos + out.nameLen > len)
        return false;
    memcpy(out.name, buf + r.pos, out.nameLen);
    out.name[out.nameLen] = '\0';
    return true;
}

size_t encodePairGrant(uint8_t *buf, size_t len, uint32_t seq, const PairGrant &m)
{
    if (m.nameLen > 11)
        return 0;
    Writer w{buf, len};
    writeHeader(w, MSG_PAIR_GRANT, seq);
    for (int i = 0; w.ok && i < 32; i++)
        w.u8(m.psk[i]);
    w.u8(m.nameLen);
    for (uint8_t i = 0; w.ok && i < m.nameLen; i++)
        w.u8((uint8_t)m.channelName[i]);
    w.u32(m.gatewayId);
    return w.ok ? w.pos : 0;
}

bool decodePairGrant(const uint8_t *buf, size_t len, PairGrant &out)
{
    Reader r = bodyReader(buf, len);
    for (int i = 0; i < 32; i++)
        out.psk[i] = r.u8();
    if (!r.ok) // buffer curto: não continuar preenchendo material de chave
        return false;
    out.nameLen = r.u8();
    if (!r.ok || out.nameLen > 11 || r.pos + out.nameLen + 4 > len)
        return false;
    memcpy(out.channelName, buf + r.pos, out.nameLen);
    out.channelName[out.nameLen] = '\0';
    r.pos += out.nameLen;
    out.gatewayId = r.u32();
    return r.ok;
}

size_t encodeEvento(uint8_t *buf, size_t len, uint32_t seq, const Evento &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_EVENTO, seq);
    w.u8(m.code);
    w.u32(m.arg);
    return w.ok ? w.pos : 0;
}

bool decodeEvento(const uint8_t *buf, size_t len, Evento &out)
{
    Reader r = bodyReader(buf, len);
    out.code = r.u8();
    out.arg = r.u32();
    return r.ok;
}

size_t encodeRemoteCmd(uint8_t *buf, size_t len, uint32_t seq, const RemoteCmd &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_REMOTE_CMD, seq);
    w.u8(m.zoneId);
    w.u8(m.action);
    w.u16(m.durationS);
    return w.ok ? w.pos : 0;
}

bool decodeRemoteCmd(const uint8_t *buf, size_t len, RemoteCmd &out)
{
    Reader r = bodyReader(buf, len);
    out.zoneId = r.u8();
    out.action = r.u8();
    out.durationS = r.u16();
    return r.ok;
}

size_t encodeCmdMaint(uint8_t *buf, size_t len, uint32_t seq, const CmdMaint &m)
{
    Writer w{buf, len};
    writeHeader(w, MSG_CMD_MAINT, seq);
    w.u16(m.durationMin);
    return w.ok ? w.pos : 0;
}

bool decodeCmdMaint(const uint8_t *buf, size_t len, CmdMaint &out)
{
    Reader r = bodyReader(buf, len);
    out.durationMin = r.u16();
    return r.ok;
}

} // namespace IrrigationProto
