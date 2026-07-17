#include "modules/irrigation/PortalApi.h"

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
    w.keyNum("numValves", ctx.numValves);
    w.keyNum("valveStates", ctx.valveStates);
    w.keyNum("gpoStates", ctx.gpoStates);
    w.keyNum("vbatCentiV", ctx.vbatCentiV);
    w.keyNum("vpanelCentiV", ctx.vpanelCentiV);
    w.keyNum("flags", ctx.flags);
    w.keyNum("apSecondsLeft", (int64_t)ctx.apSecondsLeft);
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

} // namespace IrrigationWeb
