#include "modules/irrigation/IrrigationWebApi.h"
#include <cstdio>
#include <cstring>

namespace IrrigationWeb
{

void JsonWriter::putc_(char c)
{
    if (_ovf) return;
    if (_len + 1 >= _cap) { _ovf = true; return; } // +1 mantém espaço pro '\0'
    _b[_len++] = c;
    _b[_len] = '\0';
}
void JsonWriter::puts_(const char *s) { while (*s) putc_(*s++); }
void JsonWriter::sep_() { if (_needComma) putc_(','); _needComma = false; }

void JsonWriter::beginObject() { sep_(); putc_('{'); }
void JsonWriter::endObject() { putc_('}'); _needComma = true; }
void JsonWriter::beginArray() { sep_(); putc_('['); }
void JsonWriter::endArray() { putc_(']'); _needComma = true; }
void JsonWriter::key(const char *k) { sep_(); putc_('"'); puts_(k); puts_("\":"); _needComma = false; }
void JsonWriter::str(const char *v)
{
    sep_(); // vírgula entre elementos (no-op após key(), que zera _needComma)
    putc_('"');
    for (const char *p = v; *p; ++p) {
        char c = *p;
        if (c == '"' || c == '\\') { putc_('\\'); putc_(c); }
        else if (c == '\n') { putc_('\\'); putc_('n'); }
        else if ((unsigned char)c < 0x20) { /* controle: descarta */ }
        else putc_(c);
    }
    putc_('"');
    _needComma = true;
}
void JsonWriter::num(int64_t v)
{
    sep_();
    char t[24];
    snprintf(t, sizeof(t), "%lld", (long long)v);
    puts_(t);
    _needComma = true;
}
void JsonWriter::boolean(bool v)
{
    sep_();
    puts_(v ? "true" : "false");
    _needComma = true;
}
void JsonWriter::raw(const char *v) { sep_(); puts_(v); _needComma = true; }
size_t JsonWriter::done() { return _ovf ? 0 : _len; }

SyncState computeSync(uint32_t desiredEpoch, uint32_t reportedEpoch, bool silent)
{
    if (silent) return SyncState::INALCANCAVEL;
    return (desiredEpoch != reportedEpoch) ? SyncState::PENDENTE : SyncState::SINCRONIZADA;
}
const char *syncLabel(SyncState s)
{
    switch (s) {
        case SyncState::SINCRONIZADA: return "sincronizada";
        case SyncState::PENDENTE: return "pendente";
        default: return "inalcancavel";
    }
}

size_t buildOverview(const OverviewCtx &ctx, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyBool("hasRtc", ctx.hasRtc);
    w.keyNum("stationCount", ctx.stationCount);
    w.keyNum("running", ctx.running);
    w.keyNum("runningZoneId", ctx.runningZoneId);
    w.keyNum("runningRemainMin", ctx.runningRemainMin);
    w.keyNum("alertCount", ctx.alertCount);
    w.keyBool("pairingPending", ctx.pairingPending);
    w.keyNum("pairingNodeId", (int64_t)ctx.pairingNodeId);
    w.keyNum("pairingSecondsLeft", ctx.pairingSecondsLeft);
    w.endObject();
    return w.done();
}

size_t buildStations(const StationView *views, size_t n, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const StationView &v = views[i];
        w.beginObject();
        w.keyNum("node", (int64_t)v.node);
        w.keyStr("name", v.name);
        w.keyStr("sync", syncLabel(v.sync));
        w.keyNum("secsSinceHeard", v.secsSinceHeard);
        w.keyNum("vbatCentiV", v.vbatCentiV);
        w.keyNum("vpanelCentiV", v.vpanelCentiV);
        w.keyNum("snrQuarterDb", v.snrQuarterDb);
        w.keyNum("rebootCount", v.rebootCount);
        w.keyNum("flags", v.flags);
        w.keyNum("lat", v.lat);
        w.keyNum("lon", v.lon);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

size_t buildZones(const ZoneTable &zones, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < zones.count(); i++) {
        const Zone *z = zones.zoneAt(i);
        if (!z) break;
        w.beginObject();
        w.keyNum("id", z->id);
        w.keyStr("name", z->name);
        w.keyNum("node", (int64_t)z->node);
        w.keyNum("tipo", z->tipo);
        w.keyNum("index", z->index);
        w.keyNum("maxMin", z->maxMin);
        w.keyNum("padraoMin", z->padraoMin);
        w.keyNum("fonteInput", z->fonteInput);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

size_t buildPrograms(const ProgramScheduler &sched, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < sched.count(); i++) {
        const Program *p = sched.programAt(i);
        if (!p) break;
        w.beginObject();
        w.keyNum("id", p->id);
        w.keyBool("enabled", p->enabled);
        w.keyNum("daysMask", p->daysMask);
        w.keyNum("startMinute", p->startMinute);
        w.key("steps");
        w.beginArray();
        for (uint8_t s = 0; s < p->stepCount && s < 8; s++) {
            w.beginObject();
            w.keyNum("zoneId", p->steps[s].zoneId);
            w.keyNum("durationMin", p->steps[s].durationMin);
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();
    return w.done();
}

} // namespace IrrigationWeb
