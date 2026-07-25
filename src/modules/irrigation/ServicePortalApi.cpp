#include "modules/irrigation/ServicePortalApi.h"
#include "modules/irrigation/ServiceBackup.h" // scanner estrutural IrrigationService::json*
#include <cstdlib>
#include <cstring>

namespace IrrigationWeb
{

namespace
{
using IrrigationService::Slice;

void writePinArray(JsonWriter &w, const char *key, const int8_t *pins, size_t n)
{
    w.key(key);
    w.beginArray();
    for (size_t i = 0; i < n; i++)
        w.num(pins[i]);
    w.endArray();
}

struct PinArrayCtx {
    int8_t *dst;
    size_t cap;
    size_t i;
};
bool fillPinCb(void *ctx, Slice elem)
{
    PinArrayCtx *c = static_cast<PinArrayCtx *>(ctx);
    if (c->i >= c->cap)
        return false;
    c->dst[c->i++] = (int8_t)strtol(elem.p, nullptr, 10); // elem.p aponta pro início do número
    return true;
}
void parsePinArray(const char *json, size_t n, const char *key, int8_t *dst, size_t cap)
{
    Slice arr;
    if (!IrrigationService::jsonMember(json, n, key, arr))
        return; // ausente → mantém out
    PinArrayCtx c{dst, cap, 0};
    IrrigationService::jsonForEachArray(arr, &c, fillPinCb);
}

struct SensorCtx {
    IrrigationSettings::SensorSlot *dst;
    size_t i;
};
bool sensorCb(void *ctx, Slice elem)
{
    SensorCtx *c = static_cast<SensorCtx *>(ctx);
    if (c->i >= IrrigationSettings::MAX_SENSORS)
        return false;
    IrrigationSettings::SensorSlot &se = c->dst[c->i++];
    int64_t v;
    if (IrrigationService::jsonInt(elem, "pino", v)) se.pino = (int8_t)v;
    if (IrrigationService::jsonInt(elem, "tipo", v)) se.tipo = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "flags", v)) se.flags = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "amostragemS", v)) se.amostragemS = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "debounceMs", v)) se.debounceMs = (uint16_t)v;
    if (IrrigationService::jsonInt(elem, "adcMin", v)) se.adcMin = (uint16_t)v;
    if (IrrigationService::jsonInt(elem, "adcMax", v)) se.adcMax = (uint16_t)v;
    if (IrrigationService::jsonInt(elem, "engMin", v)) se.engMin = (int16_t)v;
    if (IrrigationService::jsonInt(elem, "engMax", v)) se.engMax = (int16_t)v;
    if (IrrigationService::jsonInt(elem, "unidade", v)) se.unidade = (uint8_t)v;
    return true;
}

struct InterlockCtx {
    IrrigationSettings::LocalInterlock *dst;
    size_t i;
};
bool interlockCb(void *ctx, Slice elem)
{
    InterlockCtx *c = static_cast<InterlockCtx *>(ctx);
    if (c->i >= IrrigationSettings::MAX_LOCAL_INTERLOCKS)
        return false;
    IrrigationSettings::LocalInterlock &il = c->dst[c->i++];
    int64_t v;
    if (IrrigationService::jsonInt(elem, "sensorIdx", v)) il.sensorIdx = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "condicao", v)) il.condicao = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "acao", v)) il.acao = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "saidasValvMask", v)) il.saidasValvMask = (uint8_t)v;
    if (IrrigationService::jsonInt(elem, "valorCenti", v)) il.valorCenti = (int32_t)v;
    if (IrrigationService::jsonInt(elem, "histereseCenti", v)) il.histereseCenti = (uint16_t)v;
    if (IrrigationService::jsonInt(elem, "saidasGpoMask", v)) il.saidasGpoMask = (uint8_t)v;
    return true;
}

struct LogBuildCtx {
    JsonWriter *w;
};
void logLineCb(void *ctx, const char *line)
{
    static_cast<LogBuildCtx *>(ctx)->w->raw(line); // cada linha já é um objeto JSON
}
} // namespace

// ── Aba Clientes ─────────────────────────────────────────────────────────────

size_t buildClientList(const IrrigationService::LightProfile *clients, size_t n, const char *activeId, char *buf,
                       size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("clients");
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const IrrigationService::LightProfile &c = clients[i];
        w.beginObject();
        w.keyStr("id", c.id);
        w.keyStr("nome", c.nome);
        w.keyStr("canal", c.canalNome);
        w.keyStr("preset", IrrigationService::presetToString(c.preset));
        w.keyNum("gateway", (int64_t)c.gateway);
        w.keyNum("estacoes", c.estacaoCount);
        bool active = activeId && activeId[0] && strcmp(activeId, c.id) == 0;
        w.keyBool("active", active);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}

ParseResult parseSelect(const char *json, size_t len, char *idOut, size_t idCap)
{
    ParseResult r;
    JsonReader rd(json, len);
    if (!rd.getStr("id", idOut, idCap) || idOut[0] == '\0')
        r.fail("id ausente");
    return r;
}

// ── Aba Rede — varredura ─────────────────────────────────────────────────────

size_t buildScanResults(const ScanResults &scan, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("nodes");
    w.beginArray();
    for (size_t i = 0; i < scan.count(); i++) {
        const ScanEntry *e = scan.at(i);
        if (!e)
            break;
        w.beginObject();
        w.keyNum("node", (int64_t)e->node);
        w.keyNum("role", e->role);
        w.keyNum("epoch", (int64_t)e->epoch);
        w.keyNum("vbat", e->vbatCentiV);
        w.keyNum("fw", e->fwVersion);
        w.keyNum("lat", (int64_t)e->lat);
        w.keyNum("lon", (int64_t)e->lon);
        w.keyNum("snr", e->snrQuarterDb);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}

// ── Aba Rede — editor de config ──────────────────────────────────────────────

size_t buildStationConfig(const IrrigationSettings &s, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    // Informativos (read-only): parse os preserva do out.
    w.keyNum("version", s.version);
    w.keyNum("role", s.role);
    w.keyNum("configEpoch", (int64_t)s.configEpoch);
    w.keyNum("boundGateway", (int64_t)s.boundGateway);
    // Escalares editáveis.
    w.keyNum("numValves", s.numValves);
    w.keyNum("hbMinutes", s.hbMinutes);
    w.keyNum("vbatMinAbrirCentiV", s.vbatMinAbrirCentiV);
    w.keyNum("maxOpenConfigS", s.maxOpenConfigS);
    w.keyNum("cmdRatePerMin", s.cmdRatePerMin);
    w.keyNum("pulseMs", s.pulseMs);
    writePinArray(w, "pinsHbridgeA", s.pinsHbridgeA, IrrigationSettings::MAX_VALVES);
    writePinArray(w, "pinsHbridgeB", s.pinsHbridgeB, IrrigationSettings::MAX_VALVES);
    writePinArray(w, "pinsDigitalIn", s.pinsDigitalIn, IrrigationSettings::MAX_DIGITAL_IN);
    w.keyNum("digitalInActiveLow", s.digitalInActiveLow);
    w.keyNum("pinBtn", s.pinBtn);
    w.keyNum("pinLed", s.pinLed);
    writePinArray(w, "pinsGpo", s.pinsGpo, IrrigationSettings::MAX_GPO);
    w.keyNum("pinTamper", s.pinTamper);
    w.keyNum("hwFlags", s.hwFlags);
    w.keyNum("latE7", (int64_t)s.latE7);
    w.keyNum("lonE7", (int64_t)s.lonE7);
    w.key("sensores");
    w.beginArray();
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        const IrrigationSettings::SensorSlot &se = s.sensores[i];
        w.beginObject();
        w.keyNum("pino", se.pino);
        w.keyNum("tipo", se.tipo);
        w.keyNum("flags", se.flags);
        w.keyNum("amostragemS", se.amostragemS);
        w.keyNum("debounceMs", se.debounceMs);
        w.keyNum("adcMin", se.adcMin);
        w.keyNum("adcMax", se.adcMax);
        w.keyNum("engMin", se.engMin);
        w.keyNum("engMax", se.engMax);
        w.keyNum("unidade", se.unidade);
        w.endObject();
    }
    w.endArray();
    w.key("localInterlocks");
    w.beginArray();
    for (uint8_t i = 0; i < IrrigationSettings::MAX_LOCAL_INTERLOCKS; i++) {
        const IrrigationSettings::LocalInterlock &il = s.localInterlocks[i];
        w.beginObject();
        w.keyNum("sensorIdx", il.sensorIdx);
        w.keyNum("condicao", il.condicao);
        w.keyNum("acao", il.acao);
        w.keyNum("saidasValvMask", il.saidasValvMask);
        w.keyNum("valorCenti", (int64_t)il.valorCenti);
        w.keyNum("histereseCenti", il.histereseCenti);
        w.keyNum("saidasGpoMask", il.saidasGpoMask);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}

ParseResult parseStationConfig(const char *json, size_t len, IrrigationSettings &out)
{
    ParseResult r;
    if (!json || len == 0) {
        r.fail("config vazia");
        return r;
    }
    Slice top{json, len};
    int64_t v;
    // Escalares editáveis (ausente → mantém out).
    if (IrrigationService::jsonInt(top, "numValves", v)) out.numValves = (uint8_t)v;
    if (IrrigationService::jsonInt(top, "hbMinutes", v)) out.hbMinutes = (uint16_t)v;
    if (IrrigationService::jsonInt(top, "vbatMinAbrirCentiV", v)) out.vbatMinAbrirCentiV = (uint16_t)v;
    if (IrrigationService::jsonInt(top, "maxOpenConfigS", v)) out.maxOpenConfigS = (uint16_t)v;
    if (IrrigationService::jsonInt(top, "cmdRatePerMin", v)) out.cmdRatePerMin = (uint8_t)v;
    if (IrrigationService::jsonInt(top, "pulseMs", v)) out.pulseMs = (uint16_t)v;
    if (IrrigationService::jsonInt(top, "digitalInActiveLow", v)) out.digitalInActiveLow = (uint8_t)v;
    if (IrrigationService::jsonInt(top, "pinBtn", v)) out.pinBtn = (int8_t)v;
    if (IrrigationService::jsonInt(top, "pinLed", v)) out.pinLed = (int8_t)v;
    if (IrrigationService::jsonInt(top, "pinTamper", v)) out.pinTamper = (int8_t)v;
    if (IrrigationService::jsonInt(top, "hwFlags", v)) out.hwFlags = (uint8_t)v;
    if (IrrigationService::jsonInt(top, "latE7", v)) out.latE7 = (int32_t)v;
    if (IrrigationService::jsonInt(top, "lonE7", v)) out.lonE7 = (int32_t)v;
    // Arrays de pinos.
    parsePinArray(json, len, "pinsHbridgeA", out.pinsHbridgeA, IrrigationSettings::MAX_VALVES);
    parsePinArray(json, len, "pinsHbridgeB", out.pinsHbridgeB, IrrigationSettings::MAX_VALVES);
    parsePinArray(json, len, "pinsDigitalIn", out.pinsDigitalIn, IrrigationSettings::MAX_DIGITAL_IN);
    parsePinArray(json, len, "pinsGpo", out.pinsGpo, IrrigationSettings::MAX_GPO);
    // Sensores + intertravamentos.
    Slice arr;
    if (IrrigationService::jsonMember(json, len, "sensores", arr)) {
        SensorCtx sc{out.sensores, 0};
        IrrigationService::jsonForEachArray(arr, &sc, sensorCb);
    }
    if (IrrigationService::jsonMember(json, len, "localInterlocks", arr)) {
        InterlockCtx ic{out.localInterlocks, 0};
        IrrigationService::jsonForEachArray(arr, &ic, interlockCb);
    }
    return r; // magic/version/role/boundGateway/configEpoch intocados (preservados)
}

// ── Aba Rede — escrita de config ─────────────────────────────────────────────

ParseResult parseNodeConfigReq(const char *json, size_t len, NodeConfigReq &out)
{
    ParseResult r;
    Slice top{json, len};
    char nodeStr[16] = {0}, route[12] = {0};
    if (!IrrigationService::jsonStr(top, "node", nodeStr, sizeof nodeStr)) {
        r.fail("node ausente");
        return r;
    }
    out.node = IrrigationService::parseNodeHex(nodeStr);
    if (out.node == 0) {
        r.fail("node invalido");
        return r;
    }
    if (!IrrigationService::jsonStr(top, "route", route, sizeof route)) {
        r.fail("route ausente");
        return r;
    }
    if (strcmp(route, "gateway") == 0)
        out.route = SvcRoute::VIA_GATEWAY;
    else if (strcmp(route, "direct") == 0)
        out.route = SvcRoute::DIRECT;
    else {
        r.fail("route invalida");
        return r;
    }
    Slice cfg;
    if (!IrrigationService::jsonMember(json, len, "config", cfg)) {
        r.fail("config ausente");
        return r;
    }
    return parseStationConfig(cfg.p, cfg.n, out.config); // preserva os geridos do out.config
}

// ── Aba Rede — ações por nó ──────────────────────────────────────────────────

ParseResult parseNodeAction(const char *json, size_t len, NodeAction &out)
{
    ParseResult r;
    Slice top{json, len};
    char nodeStr[16] = {0}, act[16] = {0};
    if (!IrrigationService::jsonStr(top, "node", nodeStr, sizeof nodeStr)) {
        r.fail("node ausente");
        return r;
    }
    out.node = IrrigationService::parseNodeHex(nodeStr);
    if (out.node == 0) {
        r.fail("node invalido");
        return r;
    }
    if (!IrrigationService::jsonStr(top, "action", act, sizeof act)) {
        r.fail("action ausente");
        return r;
    }
    int64_t v;
    if (strcmp(act, "pulse") == 0) {
        out.action = SvcAction::PULSE;
        if (!IrrigationService::jsonInt(top, "valveId", v) || v < 0 || v > 7) {
            r.fail("valveId 0..7");
            return r;
        }
        out.valveOrZoneId = (uint8_t)v;
        if (!IrrigationService::jsonInt(top, "durationS", v) || v < 1 || v > 7200) {
            r.fail("durationS 1..7200");
            return r;
        }
        out.durationS = (uint16_t)v;
    } else if (strcmp(act, "zone") == 0) {
        out.action = SvcAction::ZONE;
        if (!IrrigationService::jsonInt(top, "zoneId", v) || v < 1 || v > 255) {
            r.fail("zoneId 1..255");
            return r;
        }
        out.valveOrZoneId = (uint8_t)v;
        if (!IrrigationService::jsonInt(top, "open", v)) {
            r.fail("open ausente");
            return r;
        }
        out.open = v != 0;
        if (out.open) {
            if (!IrrigationService::jsonInt(top, "durationS", v) || v < 1 || v > 7200) {
                r.fail("durationS 1..7200");
                return r;
            }
            out.durationS = (uint16_t)v;
        } else {
            out.durationS = 0;
        }
    } else if (strcmp(act, "approve_pair") == 0) {
        out.action = SvcAction::APPROVE_PAIR;
    } else if (strcmp(act, "resync") == 0) {
        out.action = SvcAction::RESYNC;
    } else {
        r.fail("action desconhecida");
        return r;
    }
    return r;
}

// ── Aba Log ──────────────────────────────────────────────────────────────────

size_t buildServiceLog(IServiceLogReader &reader, size_t maxLines, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("log");
    w.beginArray();
    LogBuildCtx c{&w};
    reader.forEachLine(maxLines, &c, logLineCb);
    w.endArray();
    w.endObject();
    return w.done();
}

} // namespace IrrigationWeb
