#include "modules/irrigation/IrrigationWebApi.h"
#include <cstdio>
#include <cstdlib>
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

void ParseResult::fail(const char *m)
{
    ok = false;
    if (errorCount < 4) {
        snprintf(errors[errorCount].msg, sizeof(errors[errorCount].msg), "%s", m);
        errorCount++;
    }
}

// Busca "key" no nível superior. Estratégia simples: procura a substring "\"key\""
// seguida de ':' e devolve o 1º char não-espaço do valor. Suficiente para objetos
// planos gerados pelo próprio painel (não é um parser JSON completo — nomes de chave
// não contêm os caracteres especiais que iludiriam a busca).
const char *JsonReader::findValue(const char *key) const
{
    char pat[40];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0) return nullptr;
    const char *p = _j;
    const char *end = _j + _len;
    while ((p = strstr(p, pat)) != nullptr && p < end) {
        const char *q = p + pn;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q < end && *q == ':') {
            q++;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            return q;
        }
        p = q;
    }
    return nullptr;
}

bool JsonReader::getInt(const char *key, int64_t &out) const
{
    const char *v = findValue(key);
    if (!v) return false;
    char *end = nullptr;
    long long n = strtoll(v, &end, 10);
    if (end == v) return false;
    out = (int64_t)n;
    return true;
}

bool JsonReader::getBool(const char *key, bool &out) const
{
    const char *v = findValue(key);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0) { out = true; return true; }
    if (strncmp(v, "false", 5) == 0) { out = false; return true; }
    return false;
}

bool JsonReader::getStr(const char *key, char *out, size_t cap) const
{
    const char *v = findValue(key);
    if (!v || *v != '"' || cap == 0) return false;
    v++; // pula a aspa de abertura
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < cap) {
        if (*v == '\\' && v[1]) v++; // escape simples: copia o próximo literal
        out[i++] = *v++;
    }
    out[i] = '\0';
    return (*v == '"'); // só ok se fechou a string
}

ParseResult parseZoneUpsert(const char *json, size_t len, Zone &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0, node = 0, tipo = 0, index = 0, maxMin = 0, padraoMin = 0, fonte = -1;
    char name[16] = {0};
    if (!rd.getInt("id", id) || id < 1 || id > 255) r.fail("id invalido (1..255)");
    if (!rd.getStr("name", name, sizeof(name)) || name[0] == '\0') r.fail("nome vazio/ausente");
    if (!rd.getInt("node", node) || node == 0) r.fail("node ausente/zero");
    if (!rd.getInt("tipo", tipo) || (tipo != 0 && tipo != 1)) r.fail("tipo deve ser 0 ou 1");
    if (!rd.getInt("index", index) || index < 0 || index > 7) r.fail("index fora de 0..7");
    if (!rd.getInt("maxMin", maxMin) || maxMin < 1 || maxMin > 120) r.fail("maxMin fora de 1..120");
    if (!rd.getInt("padraoMin", padraoMin) || padraoMin < 1 || padraoMin > maxMin) r.fail("padraoMin fora de 1..maxMin");
    rd.getInt("fonteInput", fonte); // opcional; default -1
    if (fonte < -1 || fonte > 3) r.fail("fonteInput fora de -1..3");
    if (!r.ok) return r;
    out = Zone{};
    out.id = (uint8_t)id;
    snprintf(out.name, sizeof(out.name), "%s", name);
    out.node = (uint32_t)node;
    out.tipo = (uint8_t)tipo;
    out.index = (uint8_t)index;
    out.maxMin = (uint16_t)maxMin;
    out.padraoMin = (uint16_t)padraoMin;
    out.fonteInput = (int8_t)fonte;
    return r;
}

ParseResult parseZoneDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > 255) { r.fail("id invalido (1..255)"); return r; }
    outId = (uint8_t)id;
    return r;
}

} // namespace IrrigationWeb
