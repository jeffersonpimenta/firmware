#include "modules/irrigation/PortalApi.h"
#include <cstring>

namespace IrrigationWeb
{

size_t buildNodeState(const NodeStateCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyNum("role", ctx.role);
    w.keyStr("name", ctx.name);
    w.keyNum("boundGateway", (int64_t)ctx.boundGateway);
    w.keyNum("configEpoch", (int64_t)ctx.configEpoch);
    w.keyBool("safeMode", ctx.safeMode);
    w.keyBool("provisioned", ctx.provisioned);
    w.keyNum("numValves", ctx.numValves);
    w.keyNum("numGpos", ctx.numGpos);
    w.keyNum("valveStates", ctx.valveStates);
    w.keyNum("gpoStates", ctx.gpoStates);
    w.keyNum("vbatCentiV", ctx.vbatCentiV);
    w.keyNum("vpanelCentiV", ctx.vpanelCentiV);
    w.keyNum("flags", ctx.flags);
    w.keyNum("apSecondsLeft", (int64_t)ctx.apSecondsLeft);
    w.keyNum("uptimeS", (int64_t)ctx.uptimeS);
    w.endObject();
    return w.done();
}

ParseResult parsePulse(const char *json, size_t len, PortalPulseReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t valveId = 0, dur = 0;
    // valveId é o índice físico da válvula local (0-based, 0..MAX_VALVES-1), não o id lógico
    // de zona do gateway (1-based). Por isso o piso é 0, diferente de parseZoneUpsert/parseCommand.
    if (!rd.getInt("valveId", valveId) || valveId < 0 || valveId > 7) r.fail("valveId fora de 0..7");
    if (!rd.getInt("durationS", dur) || dur < 1 || dur > 7200) r.fail("durationS fora de 1..7200");
    if (!r.ok) return r;
    out.valveId = (uint8_t)valveId;
    out.durationS = (uint16_t)dur;
    return r;
}

ParseResult parseProvision(const char *json, size_t len, ProvisionReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t role = 0;
    if (!rd.getInt("role", role) || role < 0 || role > 3) {
        r.fail("role fora de 0..3");
        return r;
    }
    out.role = (uint8_t)role;
    // farmName é opcional; ausência não é erro (só usado no papel GATEWAY p/ nomear o canal).
    char fn[13] = {0};
    if (rd.getStr("farmName", fn, sizeof(fn))) {
        memcpy(out.farmName, fn, sizeof(out.farmName));
        out.hasFarmName = fn[0] != '\0';
    }
    return r;
}

ParseResult parseNetCommand(const char *json, size_t len, NetCommand &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    char kind[12] = {0};
    int64_t zoneId = 0, dur = 0;
    if (!rd.getStr("kind", kind, sizeof(kind))) { r.fail("kind ausente"); return r; }
    if (!rd.getInt("zoneId", zoneId) || zoneId < 1 || zoneId > 255) r.fail("zoneId fora de 1..255");
    if (strcmp(kind, "open") == 0) {
        if (!rd.getInt("durationS", dur) || dur < 1 || dur > 7200) r.fail("durationS fora de 1..7200");
        out.action = 1;
    } else if (strcmp(kind, "close") == 0) {
        out.action = 0;
        dur = 0;
    } else {
        r.fail("kind desconhecido");
    }
    if (!r.ok) return r;
    out.zoneId = (uint8_t)zoneId;
    out.durationS = (uint16_t)dur;
    return r;
}

size_t buildRoster(const ZoneTable *zones, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    if (zones) {
        for (size_t i = 0; i < zones->count(); i++) {
            const Zone *z = zones->zoneAt(i);
            if (!z) break;
            w.beginObject();
            w.keyNum("id", z->id);
            w.keyStr("name", z->name);
            w.keyNum("padraoMin", z->padraoMin);
            w.endObject();
        }
    }
    w.endArray();
    return w.done();
}

size_t buildSensors(const PortalSensorsCtx &ctx, char *buf, size_t cap)
{
    static const char *UNIT_NAMES[] = {"", "bar", "%", "m", "C"};
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("sensors");
    w.beginArray();
    uint8_t n = ctx.count < 4 ? ctx.count : 4;
    for (uint8_t i = 0; i < n; i++) {
        const PortalSensorItem &it = ctx.items[i];
        w.beginObject();
        w.keyNum("id", it.id);
        w.keyNum("tipo", it.tipo);
        const char *uname = (it.unidade < 5) ? UNIT_NAMES[it.unidade] : "";
        w.keyStr("unidade", uname);
        w.keyNum("valor", it.valueCenti);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}

size_t buildPortalLog(const AuditLog &log, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.key("log");
    w.beginArray();
    size_t total = log.size();
    if (total > 100) total = 100;
    for (size_t i = 0; i < total; i++) {
        const AuditRecord &rec = log.at(i);
        w.beginObject();
        w.keyNum("ts", (int64_t)rec.tsSecs);
        w.keyNum("origem", rec.origin);
        w.keyNum("acao", rec.action);
        w.keyNum("alvo", rec.target);
        w.keyNum("res", rec.result);
        w.keyNum("no", (int64_t)rec.node);
        w.keyNum("seq", (int64_t)rec.seq);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}

ParseResult parseGpoReq(const char *json, size_t len, PortalGpoReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t gpo = 0, action = 0, dur = 0;
    if (!rd.getInt("gpo", gpo) || gpo < 0 || gpo > 1) r.fail("gpo fora de 0..1");
    if (!rd.getInt("action", action) || action < 0 || action > 1) r.fail("action fora de 0..1");
    if (!rd.getInt("durationS", dur) || dur < 0 || dur > 7200) r.fail("durationS fora de 0..7200");
    if (!r.ok) return r;
    out.gpoId = (uint8_t)gpo;
    out.action = (uint8_t)action;
    out.durationS = (uint16_t)dur;
    bool confirm = false;
    rd.getBool("confirm", confirm); // ausente => false, não é erro
    out.confirm = confirm;
    return r;
}

size_t buildCoords(const PortalCoords &c, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyNum("latE7", (int64_t)c.latE7);
    w.keyNum("lonE7", (int64_t)c.lonE7);
    w.endObject();
    return w.done();
}

ParseResult parseCoords(const char *json, size_t len, PortalCoords &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t lat = 0, lon = 0;
    if (!rd.getInt("latE7", lat)) { r.fail("latE7 ausente"); }
    else if (lat < -900000000LL || lat > 900000000LL) { r.fail("latE7 fora de range"); }
    if (!rd.getInt("lonE7", lon)) { r.fail("lonE7 ausente"); }
    else if (lon < -1800000000LL || lon > 1800000000LL) { r.fail("lonE7 fora de range"); }
    if (!r.ok) return r;
    out.latE7 = (int32_t)lat;
    out.lonE7 = (int32_t)lon;
    return r;
}

size_t buildLink(const LinkCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyNum("snrQuarterDb", (int64_t)ctx.snrQuarterDb);
    w.keyNum("rssiDbm", (int64_t)ctx.rssiDbm);
    w.key("history");
    w.beginArray();
    for (uint8_t i = 0; i < ctx.histCount; i++)
        w.num((int64_t)ctx.hist[i]);
    w.endArray();
    w.key("neighbors");
    w.beginArray();
    for (uint8_t i = 0; i < ctx.neighborCount; i++) {
        const LinkNeighbor &n = ctx.neighbors[i];
        w.beginObject();
        w.keyNum("node", (int64_t)n.node);
        w.keyNum("snrQuarterDb", (int64_t)n.snrQuarterDb);
        w.keyNum("hops", (int64_t)n.hops);
        w.keyStr("name", n.name);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}

size_t buildWifiStatus(const WifiStatusCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyBool("enabled", ctx.enabled);
    w.keyBool("staUp", ctx.staUp);
    w.keyStr("connectedSsid", ctx.connectedSsid);
    w.keyStr("ip", ctx.ip);
    w.endObject();
    return w.done();
}

size_t buildWifiScan(const WifiScanCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyBool("scanning", ctx.scanning);
    w.key("networks");
    w.beginArray();
    if (!ctx.scanning) {
        uint8_t n = ctx.count > 16 ? 16 : ctx.count;
        for (uint8_t i = 0; i < n; i++) {
            w.beginObject();
            w.keyStr("ssid", ctx.items[i].ssid);
            w.keyNum("rssi", ctx.items[i].rssi);
            w.keyBool("secure", ctx.items[i].secure);
            w.endObject();
        }
    }
    w.endArray();
    w.endObject();
    return w.done();
}

ParseResult parseWifiConnect(const char *json, size_t len, WifiConnectReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    char ssid[33] = {0};
    char psk[64] = {0};
    if (!rd.getStr("ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        r.fail("ssid vazio");
        return r;
    }
    rd.getStr("psk", psk, sizeof(psk)); // opcional (rede aberta = vazio)
    size_t plen = strlen(psk);
    if (plen > 0 && plen < 8) {
        r.fail("senha < 8 caracteres");
        return r;
    }
    memcpy(out.ssid, ssid, sizeof(out.ssid));
    memcpy(out.psk, psk, sizeof(out.psk));
    return r;
}

size_t buildWifiConnect(const WifiConnectCtx &ctx, char *buf, size_t cap)
{
    const char *s = "idle";
    switch (ctx.state) {
    case WifiConnectState::Connecting: s = "connecting"; break;
    case WifiConnectState::Success:    s = "success";    break;
    case WifiConnectState::Error:      s = "error";      break;
    default:                           s = "idle";       break;
    }
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyStr("state", s);
    w.keyStr("ssid", ctx.ssid);
    w.keyStr("error", ctx.error);
    w.endObject();
    return w.done();
}

ParseResult parseWifiToggle(const char *json, size_t len, WifiToggleReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    bool en = false;
    if (!rd.getBool("enabled", en)) {
        r.fail("campo enabled ausente");
        return r;
    }
    out.enabled = en;
    return r;
}

} // namespace IrrigationWeb
