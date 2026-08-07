#include "modules/irrigation/IrrigationWebApi.h"
#include "modules/irrigation/ServiceBackup.h"
#include "modules/irrigation/WeatherEngine.h"
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

const TzPreset TZ_PRESETS[] = {
    {"America/Sao_Paulo", "<-03>3"},
    {"America/Manaus", "<-04>4"},
    {"America/Rio_Branco", "<-05>5"},
    {"America/Noronha", "<-02>2"},
    {"UTC", "GMT0"},
};
const size_t TZ_PRESETS_COUNT = sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]);

const char *tzLabelFor(const char *posix)
{
    if (posix)
        for (size_t i = 0; i < TZ_PRESETS_COUNT; i++)
            if (strcmp(posix, TZ_PRESETS[i].posix) == 0)
                return TZ_PRESETS[i].label;
    return "Personalizado";
}
bool tzIsValidPreset(const char *posix)
{
    if (!posix) return false;
    for (size_t i = 0; i < TZ_PRESETS_COUNT; i++)
        if (strcmp(posix, TZ_PRESETS[i].posix) == 0)
            return true;
    return false;
}

size_t buildTimeStatus(const TimeStatusCtx &ctx, char *buf, size_t cap)
{
    const char *source = ctx.quality >= 3 ? "ntp" : (ctx.nowEpoch != 0 ? "manual" : "none");
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyNum("nowEpoch", (int64_t)ctx.nowEpoch);
    w.keyBool("hasRtc", ctx.nowEpoch != 0);
    w.keyStr("source", source);
    w.keyNum("quality", ctx.quality);
    w.keyStr("ntpServer", ctx.ntpServer ? ctx.ntpServer : "");
    w.keyNum("lastSyncS", ctx.lastSyncS);
    w.keyStr("tz", ctx.tz ? ctx.tz : "");
    w.keyStr("tzLabel", tzLabelFor(ctx.tz));
    w.keyBool("staUp", ctx.staUp);
    w.endObject();
    return w.done();
}

ParseResult parseTimeSet(const char *json, size_t len, uint32_t &epochOut)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t epoch = 0;
    if (!rd.getInt("epoch", epoch)) { r.fail("epoch ausente"); return r; }
    if (epoch < 1600000000LL || epoch > 4102444800LL) { r.fail("epoch fora de range"); return r; }
    epochOut = (uint32_t)epoch;
    return r;
}

ParseResult parseTimezone(const char *json, size_t len, char *out, size_t outCap)
{
    ParseResult r;
    JsonReader rd(json, len);
    char tz[40] = {0};
    if (!rd.getStr("tz", tz, sizeof(tz)) || tz[0] == '\0') { r.fail("tz ausente"); return r; }
    if (!tzIsValidPreset(tz)) { r.fail("fuso desconhecido"); return r; }
    strncpy(out, tz, outCap - 1);
    out[outCap - 1] = '\0';
    return r;
}

size_t buildAlerts(const AlertCenter &ac, uint32_t nowMs, uint32_t ackMs, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < ac.count(); i++) {
        const Alert &a = ac.at(i);
        if (a.type == AlertType::NONE)
            continue;
        if (a.acked)
            continue; // reconhecido individualmente pelo painel (ackMatch)
        if (a.atMs <= ackMs)
            continue; // reconhecido em massa (ACK_ALERT sem identidade registrou lastAckAllMs)
        uint32_t ageS = nowMs >= a.atMs ? (nowMs - a.atMs) / 1000 : 0;
        w.beginObject();
        w.keyNum("type", (int64_t)(int)a.type);
        w.keyNum("node", (int64_t)a.node);
        w.keyNum("arg", (int64_t)a.arg);
        w.keyNum("atMs", (int64_t)a.atMs); // identidade ecoada pelo botão "Reconhecer" (ack por-alerta)
        w.keyNum("ageS", (int64_t)ageS);
        w.endObject();
    }
    w.endArray();
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
        w.keyNum("rssiDbm", v.rssiDbm);
        w.keyNum("rebootCount", v.rebootCount);
        w.keyNum("flags", v.flags);
        w.keyNum("lat", v.lat);
        w.keyNum("lon", v.lon);
        w.keyNum("hbMinutes", v.hbMinutes);
        w.keyNum("vbatAvisoCentiV", v.vbatAvisoCentiV);
        w.keyNum("vbatCriticaCentiV", v.vbatCriticaCentiV);
        w.key("outputs");
        w.beginArray();
        for (uint8_t k = 0; k < v.outputCount && k < StationView::MAX_OUTPUTS; k++) {
            w.beginObject();
            w.keyNum("tipo", v.outputs[k].tipo);
            w.keyNum("index", v.outputs[k].index);
            w.endObject();
        }
        w.endArray();
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
    size_t patLen = (size_t)pn;
    const char *end = _j + _len;
    // Busca limitada a [_j, _j+_len): compara pat sem passar de end (não usa strstr,
    // que varreria até o NUL ignorando _len).
    for (const char *p = _j; p + patLen <= end; ++p) {
        if (memcmp(p, pat, patLen) != 0) continue;
        const char *q = p + patLen;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q < end && *q == ':') {
            q++;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end) return q; // 1º char do valor, ainda dentro dos limites
        }
        // pat casou mas não formava "chave": pula ao próximo char após o pat.
        p = q - 1; // -1 compensa o ++p do laço
    }
    return nullptr;
}

bool JsonReader::getInt(const char *key, int64_t &out) const
{
    const char *v = findValue(key);
    if (!v) return false;
    const char *end = _j + _len;
    // Copia o token numérico (sinal opcional + dígitos) que cabe em [v, end) para um
    // buffer local NUL-terminado; strtoll não pode ultrapassar _len.
    char tok[24];
    size_t i = 0;
    const char *p = v;
    if (p < end && (*p == '+' || *p == '-') && i + 1 < sizeof(tok)) tok[i++] = *p++;
    bool anyDigit = false;
    while (p < end && *p >= '0' && *p <= '9' && i + 1 < sizeof(tok)) {
        tok[i++] = *p++;
        anyDigit = true;
    }
    if (!anyDigit) return false;
    tok[i] = '\0';
    char *tend = nullptr;
    long long n = strtoll(tok, &tend, 10);
    if (tend == tok) return false;
    out = (int64_t)n;
    return true;
}

bool JsonReader::getBool(const char *key, bool &out) const
{
    const char *v = findValue(key);
    if (!v) return false;
    const char *end = _j + _len;
    if ((size_t)(end - v) >= 4 && memcmp(v, "true", 4) == 0) { out = true; return true; }
    if ((size_t)(end - v) >= 5 && memcmp(v, "false", 5) == 0) { out = false; return true; }
    return false;
}

bool JsonReader::getStr(const char *key, char *out, size_t cap) const
{
    const char *v = findValue(key);
    const char *end = _j + _len;
    if (!v || v >= end || *v != '"' || cap == 0) return false;
    v++; // pula a aspa de abertura
    size_t i = 0;
    while (v < end && *v != '"' && i + 1 < cap) {
        if (*v == '\\' && v + 1 < end) v++; // escape simples: copia o próximo literal
        out[i++] = *v++;
    }
    out[i] = '\0';
    return (v < end && *v == '"'); // só ok se fechou a string dentro dos limites
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

ParseResult parseProgramUpsert(const char *json, size_t len, Program &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0, days = 0, start = 0;
    bool enabled = true;
    if (!rd.getInt("id", id) || id < 1 || id > 255) r.fail("id invalido (1..255)");
    if (!rd.getInt("daysMask", days) || days < 0 || days > 127) r.fail("daysMask fora de 0..127");
    if (!rd.getInt("startMinute", start) || start < 0 || start > 1439) r.fail("startMinute fora de 0..1439");
    rd.getBool("enabled", enabled); // opcional

    // Localiza o array "steps".
    const char *sp = strstr(json, "\"steps\"");
    const char *arr = sp ? strchr(sp, '[') : nullptr;
    const char *arrEnd = arr ? strchr(arr, ']') : nullptr;
    if (!arr || !arrEnd) r.fail("steps ausente");
    Program p = Program{};
    uint8_t count = 0;
    if (arr && arrEnd) {
        const char *o = arr;
        while ((o = strchr(o, '{')) != nullptr && o < arrEnd) {
            const char *oEnd = strchr(o, '}');
            if (!oEnd || oEnd > arrEnd) break;
            if (count >= 8) { r.fail("mais de 8 etapas"); break; }
            JsonReader sr(o, (size_t)(oEnd - o + 1));
            int64_t zoneId = 0, dur = 0;
            if (!sr.getInt("zoneId", zoneId) || zoneId < 1 || zoneId > 255) { r.fail("zoneId de etapa invalido"); break; }
            if (!sr.getInt("durationMin", dur) || dur < 1 || dur > 120) { r.fail("durationMin de etapa fora de 1..120"); break; }
            p.steps[count].zoneId = (uint8_t)zoneId;
            p.steps[count].durationMin = (uint16_t)dur;
            count++;
            o = oEnd + 1;
        }
    }
    if (count == 0) r.fail("programa sem etapas");
    if (!r.ok) return r;
    p.id = (uint8_t)id;
    p.enabled = enabled;
    p.daysMask = (uint8_t)days;
    p.startMinute = (uint16_t)start;
    p.stepCount = count;
    out = p;
    return r;
}

ParseResult parseProgramToggle(const char *json, size_t len, uint8_t &outId, bool &outEnabled)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    bool en = false;
    if (!rd.getInt("id", id) || id < 1 || id > 255) r.fail("id invalido (1..255)");
    if (!rd.getBool("enabled", en)) r.fail("enabled ausente");
    if (!r.ok) return r;
    outId = (uint8_t)id;
    outEnabled = en;
    return r;
}

ParseResult parseProgramDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > 255) { r.fail("id invalido (1..255)"); return r; }
    outId = (uint8_t)id;
    return r;
}

ParseResult parseCommand(const char *json, size_t len, WebCommand &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    char kind[20] = {0};
    if (!rd.getStr("kind", kind, sizeof(kind))) { r.fail("kind ausente"); return r; }
    WebCommand c = WebCommand{};
    int64_t zoneId = 0, dur = 0, node = 0, atype = 0, arg = 0, atMs = 0;
    rd.getInt("zoneId", zoneId);
    rd.getInt("durationS", dur);
    rd.getInt("node", node);
    rd.getInt("type", atype);  // ACK_ALERT: identidade do alerta (ausente ⇒ 0)
    rd.getInt("arg", arg);
    rd.getInt("atMs", atMs);
    c.zoneId = (uint8_t)zoneId;
    c.node = (uint32_t)node;
    c.alertType = (uint8_t)atype;
    c.arg = (uint32_t)arg;
    c.atMs = (uint32_t)atMs;
    if (strcmp(kind, "pulse") == 0) {
        if (zoneId < 1 || zoneId > 255) r.fail("zoneId invalido");
        c.kind = CmdKind::PULSE_TEST;
        c.durationS = 10; // §7.1: teste de pulso abre 10 s
    } else if (strcmp(kind, "open") == 0) {
        if (zoneId < 1 || zoneId > 255) r.fail("zoneId invalido");
        if (dur < 1 || dur > 7200) r.fail("durationS fora de 1..7200");
        c.kind = CmdKind::OPEN;
        c.durationS = (uint16_t)dur;
    } else if (strcmp(kind, "close") == 0) {
        if (zoneId < 1 || zoneId > 255) r.fail("zoneId invalido");
        c.kind = CmdKind::CLOSE;
    } else if (strcmp(kind, "ack") == 0) {
        c.kind = CmdKind::ACK_ALERT; // node/zoneId identificam o alerta
    } else if (strcmp(kind, "approve_pairing") == 0) {
        c.kind = CmdKind::APPROVE_PAIRING;
    } else {
        r.fail("kind desconhecido");
    }
    if (!r.ok) return r;
    out = c;
    return r;
}

// ── Fase 9 — parsers de estação ───────────────────────────────────────────────

ParseResult parseStationConfig(const char *json, size_t len, StationConfigReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t node = 0, hb = -1, aviso = -1, critica = -1, latE7 = 0, lonE7 = 0;
    if (!rd.getInt("node", node) || node == 0) r.fail("node ausente/zero");
    if (!rd.getInt("hbMinutes", hb) || hb < 1 || hb > 1440) r.fail("hbMinutes fora de 1..1440");
    if (!rd.getInt("vbatAvisoCentiV", aviso) || aviso < 800 || aviso > 1500) r.fail("vbatAvisoCentiV fora de 800..1500");
    if (!rd.getInt("vbatCriticaCentiV", critica) || critica < 800 || critica > 1500)
        r.fail("vbatCriticaCentiV fora de 800..1500");
    if (r.ok && !(aviso > critica)) r.fail("aviso deve ser maior que critica");
    rd.getInt("latE7", latE7); // coords opcionais (0 = sem coordenada)
    rd.getInt("lonE7", lonE7);
    if (latE7 < -900000000 || latE7 > 900000000) r.fail("latE7 fora de faixa");
    if (lonE7 < -1800000000 || lonE7 > 1800000000) r.fail("lonE7 fora de faixa");
    if (!r.ok) return r;
    out.node = (uint32_t)node;
    out.hbMinutes = (uint16_t)hb;
    out.vbatAvisoCentiV = (uint16_t)aviso;
    out.vbatCriticaCentiV = (uint16_t)critica;
    out.latE7 = (int32_t)latE7;
    out.lonE7 = (int32_t)lonE7;
    return r;
}

ParseResult parseStationDelete(const char *json, size_t len, uint32_t &outNode)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t node = 0;
    if (!rd.getInt("node", node) || node == 0) r.fail("node ausente/zero");
    if (!r.ok) return r;
    outNode = (uint32_t)node;
    return r;
}

ParseResult parseStationPulse(const char *json, size_t len, StationPulseReq &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t node = 0, tipo = 0, index = -1, dur = 10;
    if (!rd.getInt("node", node) || node == 0) r.fail("node ausente/zero");
    if (!rd.getInt("tipo", tipo) || tipo < 0 || tipo > 1) r.fail("tipo fora de 0..1");
    if (!rd.getInt("index", index) || index < 0 || index > 7) r.fail("index fora de 0..7");
    rd.getInt("durationS", dur);
    if (dur < 1 || dur > 120) dur = 10; // teste de pulso: default/teto seguro
    if (!r.ok) return r;
    out.node = (uint32_t)node;
    out.tipo = (uint8_t)tipo;
    out.index = (uint8_t)index;
    out.durationS = (uint16_t)dur;
    return r;
}

// ── Fase 6b — buildInterlocks ─────────────────────────────────────────────────

size_t buildInterlocks(const InterlockTable &tbl, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < tbl.count(); i++) {
        const InterlockRule *r = tbl.ruleAt(i);
        if (!r) break;
        w.beginObject();
        w.keyNum("id", r->id);
        w.keyNum("tipo", r->tipo);
        w.keyNum("node", (int64_t)(uint32_t)r->node); // uint32 → emite sem sinal
        w.keyNum("sensor", r->sensorIdx);
        w.keyNum("condicao", r->condicao);
        w.keyNum("valor", r->valorCenti);
        w.keyNum("histerese", r->histereseCenti);
        w.keyNum("acao", r->acao);
        // array de zonas: emite apenas os ids não-zero
        w.key("zonas");
        w.beginArray();
        for (uint8_t z = 0; z < 8 && r->zoneIds[z] != 0; z++) {
            w.num(r->zoneIds[z]);
        }
        w.endArray();
        w.keyBool("todas", r->todas);
        w.keyStr("mensagem", r->mensagem);
        w.keyNum("maxAbertas", r->maxAbertas);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

// ── Fase 6b — parseInterlockUpsert ──────────────────────────────────────────

ParseResult parseInterlockUpsert(const char *json, size_t len, InterlockRule &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0, tipo = 0, node = 0, sensor = 0, cond = 0;
    int64_t valor = 0, hist = 0, acao = 0, maxAb = 0;
    bool todas = false;
    char mensagem[24] = {0};

    if (!rd.getInt("id", id) || id < 1 || id > 255) r.fail("id invalido (1..255)");
    rd.getInt("tipo", tipo);
    rd.getInt("node", node);
    rd.getInt("sensor", sensor);
    rd.getInt("condicao", cond);
    rd.getInt("valor", valor);
    rd.getInt("histerese", hist);
    rd.getInt("acao", acao);
    rd.getBool("todas", todas);
    rd.getInt("maxAbertas", maxAb);
    rd.getStr("mensagem", mensagem, sizeof(mensagem));

    // Lê array "zonas" replicando a técnica de parseProgramUpsert.
    const char *sp = strstr(json, "\"zonas\"");
    const char *arr = sp ? strchr(sp, '[') : nullptr;
    const char *arrEnd = arr ? strchr(arr, ']') : nullptr;
    uint8_t zoneIds[8] = {0};
    uint8_t zCount = 0;
    if (arr && arrEnd) {
        const char *o = arr + 1; // pula '['
        while (o < arrEnd) {
            // avança até dígito
            while (o < arrEnd && (*o < '0' || *o > '9')) o++;
            if (o >= arrEnd) break;
            JsonReader sr(o - 1, (size_t)(arrEnd - o + 2)); // slice contendo o número
            // lê número diretamente via strtoll-like: o aponta pro dígito
            int64_t zid = 0;
            const char *p2 = o;
            bool anyD = false;
            char tok[8]; size_t ti = 0;
            while (p2 < arrEnd && *p2 >= '0' && *p2 <= '9' && ti + 1 < sizeof(tok)) {
                tok[ti++] = *p2++;
                anyD = true;
            }
            if (anyD) {
                tok[ti] = '\0';
                char *tend = nullptr;
                zid = strtoll(tok, &tend, 10);
                if (tend != tok && zid > 0 && zCount < 8) {
                    zoneIds[zCount++] = (uint8_t)zid;
                }
            }
            o = p2;
            // avança até próxima vírgula ou fim
            while (o < arrEnd && *o != ',') o++;
            if (o < arrEnd) o++; // pula ','
        }
    }

    if (!r.ok) return r;

    out = InterlockRule{};
    out.id = (uint8_t)id;
    out.tipo = (uint8_t)tipo;
    out.node = (uint32_t)node;
    out.sensorIdx = (uint8_t)sensor;
    out.condicao = (uint8_t)cond;
    out.valorCenti = (int32_t)valor;
    out.histereseCenti = (uint16_t)hist;
    out.acao = (uint8_t)acao;
    out.todas = todas;
    out.maxAbertas = (uint8_t)maxAb;
    snprintf(out.mensagem, sizeof(out.mensagem), "%s", mensagem);
    for (uint8_t z = 0; z < 8; z++) out.zoneIds[z] = (z < zCount) ? zoneIds[z] : 0;
    return r;
}

// ── Fase 6b — parseInterlockDelete ──────────────────────────────────────────

ParseResult parseInterlockDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > 255) { r.fail("id invalido (1..255)"); return r; }
    outId = (uint8_t)id;
    return r;
}

// ── Fase 6b — buildSensorsGateway ────────────────────────────────────────────

size_t buildSensorsGateway(const GwStationSensors *views, size_t n, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const GwStationSensors &st = views[i];
        w.beginObject();
        w.keyNum("node", (int64_t)(uint32_t)st.node);
        w.keyStr("nome", st.stationName ? st.stationName : "");
        w.keyBool("tamper", st.tamper);
        w.key("sensores");
        w.beginArray();
        for (uint8_t k = 0; k < st.count && k < IrrigationProto::HB_MAX_SENSORS; k++) {
            const GwSensorItem &it = st.itens[k];
            w.beginObject();
            w.keyNum("idx", it.idx);
            w.keyNum("tipo", it.tipo);
            w.keyNum("valor", it.valueCenti);
            w.keyStr("nome", it.name ? it.name : "");
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();
    return w.done();
}

// ── Fase 6b — parseMaintWindow ────────────────────────────────────────────────

ParseResult parseMaintWindow(const char *json, size_t len, uint32_t &outNode, uint16_t &outMinutes)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t node = 0, minutes = -1;
    if (!rd.getInt("node", node) || node == 0) r.fail("node ausente/zero");
    if (!rd.getInt("minutes", minutes) || minutes < 0 || minutes > 1440) r.fail("minutes fora de 0..1440");
    if (!r.ok) return r;
    outNode = (uint32_t)node;
    outMinutes = (uint16_t)minutes;
    return r;
}

// ── Fase 6b — parseSensorName ─────────────────────────────────────────────────

ParseResult parseSensorName(const char *json, size_t len,
                            uint32_t &outNode, uint8_t &outIdx,
                            char *outName, size_t nameCap)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t node = 0, sensor = 0;
    char nome[64] = {0};
    if (!rd.getInt("node", node) || node == 0) r.fail("node ausente/zero");
    if (!rd.getInt("sensor", sensor) || sensor < 0 || sensor > 3) r.fail("sensor fora de 0..3");
    if (!rd.getStr("nome", nome, sizeof(nome))) r.fail("nome ausente");
    if (!r.ok) return r;
    outNode = (uint32_t)node;
    outIdx = (uint8_t)sensor;
    if (nameCap > 0) {
        size_t src = strlen(nome);
        size_t cpy = src < nameCap - 1 ? src : nameCap - 1;
        memcpy(outName, nome, cpy);
        outName[cpy] = '\0';
    }
    return r;
}

// ── Fase 7b — grupos hidráulicos: parsers ────────────────────────────────────

ParseResult parseGroupUpsert(const char *json, size_t len, HydraulicGroup &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = -1, bomba = 0, minOpen = 1, maxOpen = 1, transicao = 0;
    int64_t overlap = 10, startAfter = 5, stopBefore = 8, minRun = 5, maxStarts = 6;
    char nome[16] = {0};

    if (!rd.getInt("id", id) || id < 0 || id > (int64_t)HydraulicGroupTable::MAX)
        r.fail("id invalido (0..8)");
    rd.getStr("nome", nome, sizeof(nome));
    rd.getInt("bombaZoneId", bomba);
    rd.getInt("minOpen", minOpen);
    rd.getInt("maxOpen", maxOpen);
    rd.getInt("transicao", transicao);
    rd.getInt("overlapS", overlap);
    rd.getInt("startAfterOpenS", startAfter);
    rd.getInt("stopBeforeCloseS", stopBefore);
    rd.getInt("minRunMin", minRun);
    rd.getInt("maxStartsHour", maxStarts);

    // array "zonas" — mesma técnica de parseInterlockUpsert
    const char *sp = strstr(json, "\"zonas\"");
    const char *arr = sp ? strchr(sp, '[') : nullptr;
    const char *arrEnd = arr ? strchr(arr, ']') : nullptr;
    uint8_t zoneIds[8] = {0};
    uint8_t zCount = 0;
    if (arr && arrEnd) {
        const char *o = arr + 1;
        while (o < arrEnd) {
            while (o < arrEnd && (*o < '0' || *o > '9')) o++;
            if (o >= arrEnd) break;
            char tok[8]; size_t ti = 0; const char *p2 = o; bool anyD = false;
            while (p2 < arrEnd && *p2 >= '0' && *p2 <= '9' && ti + 1 < sizeof(tok)) {
                tok[ti++] = *p2++; anyD = true;
            }
            if (anyD) {
                tok[ti] = '\0';
                char *tend = nullptr;
                int64_t zid = strtoll(tok, &tend, 10);
                if (tend != tok && zid > 0 && zCount < 8) zoneIds[zCount++] = (uint8_t)zid;
            }
            o = p2;
            while (o < arrEnd && *o != ',') o++;
            if (o < arrEnd) o++;
        }
    }

    if (zCount < 1) r.fail("grupo sem zonas");
    if (minOpen < 1) r.fail("minOpen >= 1");
    if (maxOpen != 0 && maxOpen < minOpen) r.fail("maxOpen < minOpen");
    if (zCount >= 1 && minOpen > zCount) r.fail("minOpen > numero de zonas");
    if (transicao != 0 && transicao != 1) r.fail("transicao invalida");

    if (!r.ok) return r;

    out = HydraulicGroup{};
    out.id = (uint8_t)id;
    snprintf(out.name, sizeof(out.name), "%s", nome);
    out.bombaZoneId = (uint8_t)bomba;
    for (uint8_t z = 0; z < 8; z++) out.zoneIds[z] = (z < zCount) ? zoneIds[z] : 0;
    out.zoneCount = zCount;
    out.minOpen = (uint8_t)minOpen;
    out.maxOpen = (uint8_t)maxOpen;
    out.transicao = (uint8_t)transicao;
    out.overlapS = (uint16_t)overlap;
    out.startAfterOpenS = (uint16_t)startAfter;
    out.stopBeforeCloseS = (uint16_t)stopBefore;
    out.minRunMin = (uint16_t)minRun;
    out.maxStartsHour = (uint8_t)maxStarts;
    return r;
}

ParseResult parseGroupDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > (int64_t)HydraulicGroupTable::MAX) {
        r.fail("id invalido (1..8)");
        return r;
    }
    outId = (uint8_t)id;
    return r;
}

ParseResult parseGroupCommand(const char *json, size_t len, uint8_t &outId, bool &outOpen, uint16_t &outDurationS)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0, dur = 0;
    char acao[8] = {0};
    if (!rd.getInt("id", id) || id < 1 || id > (int64_t)HydraulicGroupTable::MAX)
        r.fail("id invalido (1..8)");
    if (!rd.getStr("acao", acao, sizeof(acao)))
        r.fail("acao ausente");
    rd.getInt("durationS", dur);

    bool open;
    if (strcmp(acao, "abrir") == 0)
        open = true;
    else if (strcmp(acao, "fechar") == 0)
        open = false;
    else {
        if (r.ok) r.fail("acao invalida (abrir|fechar)");
        return r;
    }
    if (!r.ok) return r;
    outId = (uint8_t)id;
    outOpen = open;
    outDurationS = (dur > 0 && dur <= 0xFFFF) ? (uint16_t)dur : 0;
    return r;
}

// ── Fase 7b — validateGroupZones ─────────────────────────────────────────────

bool validateGroupZones(const HydraulicGroup &g, const ZoneTable &zones, char *err, size_t errCap)
{
    for (uint8_t i = 0; i < g.zoneCount && i < 8; i++) {
        uint8_t zid = g.zoneIds[i];
        const Zone *z = zones.byId(zid);
        if (!z) {
            snprintf(err, errCap, "zona %u inexistente", zid);
            return false;
        }
        if (z->fonteInput >= 0) {
            snprintf(err, errCap, "zona %u e espelho", zid);
            return false;
        }
        if (g.bombaZoneId != 0 && zid == g.bombaZoneId) {
            snprintf(err, errCap, "bomba %u nao pode ser membro", zid);
            return false;
        }
    }
    if (g.bombaZoneId != 0 && !zones.byId(g.bombaZoneId)) {
        snprintf(err, errCap, "bomba %u inexistente", g.bombaZoneId);
        return false;
    }
    return true;
}

// ── Fase 7b — groupStateLabel / buildGroups / buildGroupsStatus ───────────────

const char *groupStateLabel(uint8_t state)
{
    switch (state) {
    case 0: return "ocioso";              // IDLE
    case 1: return "abrindo";             // OPENING
    case 2: return "aguardando_partida";  // START_WAIT
    case 3: return "partindo_bomba";      // PUMP_WAIT_ACK
    case 4: return "rodando";             // RUNNING
    case 5:                               // X_OPEN_WAIT
    case 6:                               // X_OVERLAP
    case 7: return "transicao";           // X_CLOSE_WAIT
    case 8: return "parando_bomba";       // PUMP_OFF_WAIT
    case 9: return "drenando";            // DRAIN
    case 10: return "fechando";           // CLOSE_LAST_WAIT
    case 11: return "adiado";             // DEFERRED
    default: return "desconhecido";
    }
}

size_t buildGroups(const HydraulicGroupTable &tbl, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < tbl.count(); i++) {
        const HydraulicGroup *g = tbl.groupAt(i);
        if (!g) break;
        w.beginObject();
        w.keyNum("id", g->id);
        w.keyStr("nome", g->name);
        w.keyNum("bombaZoneId", g->bombaZoneId);
        w.key("zonas");
        w.beginArray();
        for (uint8_t z = 0; z < g->zoneCount && z < 8; z++) w.num(g->zoneIds[z]);
        w.endArray();
        w.keyNum("minOpen", g->minOpen);
        w.keyNum("maxOpen", g->maxOpen);
        w.keyNum("transicao", g->transicao);
        w.keyNum("overlapS", g->overlapS);
        w.keyNum("startAfterOpenS", g->startAfterOpenS);
        w.keyNum("stopBeforeCloseS", g->stopBeforeCloseS);
        w.keyNum("minRunMin", g->minRunMin);
        w.keyNum("maxStartsHour", g->maxStartsHour);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

size_t buildGroupsStatus(const GroupStatusView *views, size_t n, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const GroupStatusView &v = views[i];
        w.beginObject();
        w.keyNum("id", v.id);
        w.keyStr("nome", v.name ? v.name : "");
        w.keyStr("estado", groupStateLabel(v.state));
        w.keyBool("bomba", v.pump);
        w.keyNum("zonaCorrente", v.curZone);
        w.keyNum("abertas", v.openCount);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

// ── Fase 8b — controle de nível (boia) ───────────────────────────────────────

size_t buildLevelControls(const LevelControlTable &tbl, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < tbl.count(); i++) {
        const LevelRule *r = tbl.ruleAt(i);
        if (!r)
            break;
        w.beginObject();
        w.keyNum("id", r->id);
        w.keyNum("sensorNode", r->sensorNode);
        w.keyNum("sensorIdx", r->sensorIdx);
        w.keyBool("ligaQuandoAtivo", r->ligaQuandoAtivo);
        w.keyNum("targetZoneId", r->targetZoneId);
        w.keyNum("minOnS", r->minOnS);
        w.keyNum("minOffS", r->minOffS);
        w.keyNum("staleTimeoutS", r->staleTimeoutS);
        w.keyStr("mensagem", r->mensagem);
        w.endObject();
    }
    w.endArray();
    return w.done();
}

ParseResult parseLevelUpsert(const char *json, size_t len, LevelRule &out)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = -1, node = 0, sidx = 0, target = 0;
    int64_t minOn = 30, minOff = 30, stale = 90;
    bool liga = true;
    char msg[24] = {0};

    if (!rd.getInt("id", id) || id < 0 || id > (int64_t)LevelControlTable::MAX)
        r.fail("id invalido (0..4)");
    rd.getInt("sensorNode", node);
    rd.getInt("sensorIdx", sidx);
    rd.getBool("ligaQuandoAtivo", liga); // aceita true/false JSON
    rd.getInt("targetZoneId", target);
    rd.getInt("minOnS", minOn);
    rd.getInt("minOffS", minOff);
    rd.getInt("staleTimeoutS", stale);
    rd.getStr("mensagem", msg, sizeof(msg));

    if (sidx < 0 || sidx > 3)
        r.fail("sensorIdx invalido (0..3)");
    if (node == 0)
        r.fail("sensorNode ausente");
    if (target < 1 || target > 255)
        r.fail("targetZoneId ausente");
    if (!r.ok)
        return r;

    out = LevelRule{};
    out.id = (uint8_t)id;
    out.sensorNode = (uint32_t)node;
    out.sensorIdx = (uint8_t)sidx;
    out.ligaQuandoAtivo = liga;
    out.targetZoneId = (uint8_t)target;
    out.minOnS = (uint16_t)(minOn < 0 ? 0 : minOn);
    out.minOffS = (uint16_t)(minOff < 0 ? 0 : minOff);
    out.staleTimeoutS = (uint16_t)(stale <= 0 ? 90 : stale);
    snprintf(out.mensagem, sizeof(out.mensagem), "%s", msg);
    return r;
}

ParseResult parseLevelDelete(const char *json, size_t len, uint8_t &outId)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t id = 0;
    if (!rd.getInt("id", id) || id < 1 || id > (int64_t)LevelControlTable::MAX) {
        r.fail("id invalido (1..4)");
        return r;
    }
    outId = (uint8_t)id;
    return r;
}

// ── Fase 8d — site survey (§8.5) ─────────────────────────────────────────────

size_t buildSurvey(const SurveyPoint *pts, size_t n, uint32_t nowS, char *buf, size_t cap)
{
    JsonWriter w(buf, cap);
    w.beginArray();
    for (size_t i = 0; i < n; i++) {
        const SurveyPoint &p = pts[i];
        w.beginObject();
        w.keyNum("no", p.node);
        w.keyNum("role", p.role);
        w.keyNum("vbat", p.vbatCentiV);
        w.keyNum("fw", p.fwVersion);
        w.keyNum("lat", p.latE7);
        w.keyNum("lon", p.lonE7);
        w.keyBool("coord", p.hasCoord);
        w.keyNum("snr", p.snrQuarterDb);
        w.keyNum("rssi", p.rssiDbm);
        w.keyNum("idadeS", (int64_t)(nowS >= p.uptimeS ? nowS - p.uptimeS : 0));
        w.endObject();
    }
    w.endArray();
    return w.done();
}

ParseResult parseSurveyStart(const char *json, size_t len, SurveyStartReq &out)
{
    ParseResult pr;
    JsonReader rd(json, len);
    int64_t v = 0;
    if (rd.getInt("intervalS", v))
        out.intervalS = (uint16_t)(v < 1 ? 1 : (v > 3600 ? 3600 : v));
    if (rd.getInt("timeoutS", v))
        out.timeoutS = (uint16_t)(v < 1 ? 1 : (v > 3600 ? 3600 : v));
    int64_t lat = 0, lon = 0;
    bool hasLat = rd.getInt("lat", lat), hasLon = rd.getInt("lon", lon);
    if (hasLat && hasLon) {
        out.latE7 = (int32_t)lat;
        out.lonE7 = (int32_t)lon;
        out.hasCoord = true;
    }
    return pr;
}

// ── Modo Espelhamento UI — web helpers ────────────────────────────────────────

ParseResult parseMirrorToggle(const char *json, size_t len, bool &enabled)
{
    ParseResult r;
    JsonReader rd(json, len);
    if (!rd.getBool("enabled", enabled)) { r.fail("falta 'enabled'"); return r; }
    return r;
}

ParseResult parseMirrorMapping(const char *json, size_t len, int8_t &input, uint8_t &zoneId,
                               bool &invertido, bool &habilitado)
{
    ParseResult r;
    JsonReader rd(json, len);
    int64_t in = -1, zid = 0;
    if (!rd.getInt("input", in) || in < 0 || in > 3) { r.fail("input fora de 0..3"); return r; }
    if (!rd.getInt("zoneId", zid) || zid < 1 || zid > 255) { r.fail("zoneId invalido"); return r; }
    bool inv = false, hab = true;
    rd.getBool("invertido", inv);   // opcional (default false)
    rd.getBool("habilitado", hab);  // opcional (default true)
    input = (int8_t)in;
    zoneId = (uint8_t)zid;
    invertido = inv;
    habilitado = hab;
    return r;
}

size_t buildMirror(char *buf, size_t cap, bool enabled, const ZoneTable &zones,
                   uint8_t digitalInActiveLow, const bool liveActive[4])
{
    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyBool("enabled", enabled);
    w.key("ports");
    w.beginArray();
    for (int i = 0; i < 4; i++) {
        const Zone *z = zones.byFonte((int8_t)i);
        w.beginObject();
        w.keyNum("i", i);
        w.keyBool("active", liveActive[i]);
        w.keyBool("invertido", ((digitalInActiveLow >> i) & 1) != 0);
        if (z) {
            w.keyNum("zoneId", z->id);
            w.keyStr("zoneName", z->name);
            w.keyBool("habilitado", z->fonteEnabled != 0);
            w.keyBool("driving", enabled && z->fonteEnabled != 0 && liveActive[i]);
        } else {
            w.keyNum("zoneId", 0);
        }
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.done();
}

// ── Sistema restore — import de tabelas de configuração (§5.5) ───────────────

namespace {

using IrrigationService::Slice;

// Copia o slice do elemento p/ buffer NUL-terminado (os parsers legados usam strstr/strchr
// sem respeitar o comprimento; sem isto podem ler além do elemento). Retorna false se não couber.
static bool sliceToBuf(Slice e, char *buf, size_t cap)
{
    if (e.n >= cap) return false;
    memcpy(buf, e.p, e.n);
    buf[e.n] = '\0';
    return true;
}

// Itera um array dentro de um sub-objeto do client, aplicando um callback por elemento.
// Retorna true mesmo que a chave esteja ausente (0 elementos = sem erro).
bool importArray(const char *obj, size_t on, const char *key,
                 void *ctx, bool (*applyElem)(void *, Slice))
{
    Slice arr;
    if (!IrrigationService::jsonMember(obj, on, key, arr))
        return true; // ausente = 0 itens, não é erro
    return IrrigationService::jsonForEachArray(arr, ctx, applyElem);
}

struct ZCtx { ZoneTable *t; uint8_t *n; };
bool applyZone(void *v, Slice e)
{
    auto *x = static_cast<ZCtx *>(v);
    char buf[1024];
    if (!sliceToBuf(e, buf, sizeof(buf))) return true; // elemento grande demais → pula
    Zone z{};
    if (parseZoneUpsert(buf, strlen(buf), z).ok && x->t->upsert(z))
        (*x->n)++;
    return true;
}

struct PCtx { ProgramScheduler *t; uint8_t *n; };
bool applyProgram(void *v, Slice e)
{
    auto *x = static_cast<PCtx *>(v);
    char buf[1024];
    if (!sliceToBuf(e, buf, sizeof(buf))) return true; // elemento grande demais → pula
    Program p{};
    if (parseProgramUpsert(buf, strlen(buf), p).ok && x->t->upsert(p))
        (*x->n)++;
    return true;
}

struct ICtx { InterlockTable *t; uint8_t *n; };
bool applyInterlock(void *v, Slice e)
{
    auto *x = static_cast<ICtx *>(v);
    char buf[1024];
    if (!sliceToBuf(e, buf, sizeof(buf))) return true; // elemento grande demais → pula
    InterlockRule r{};
    if (parseInterlockUpsert(buf, strlen(buf), r).ok && x->t->upsert(r))
        (*x->n)++;
    return true;
}

struct GCtx { HydraulicGroupTable *t; uint8_t *n; };
bool applyGroup(void *v, Slice e)
{
    auto *x = static_cast<GCtx *>(v);
    char buf[1024];
    if (!sliceToBuf(e, buf, sizeof(buf))) return true; // elemento grande demais → pula
    HydraulicGroup g{};
    if (parseGroupUpsert(buf, strlen(buf), g).ok && x->t->upsert(g))
        (*x->n)++;
    return true;
}

// Callback de envelopeForEachClient: captura apenas o primeiro client e para.
struct FirstClientCtx { Slice *dst; };
bool firstClientCb(void *c, Slice cl)
{
    auto *ctx = static_cast<FirstClientCtx *>(c);
    *ctx->dst = cl;
    return false; // para após o 1º
}

} // anonymous namespace

bool importConfigTablesFromBackup(const char *json, size_t len, ZoneTable &zones,
                                  ProgramScheduler &sched, InterlockTable &interlocks,
                                  HydraulicGroupTable &groups, ImportCounts &out,
                                  char *err, size_t errCap)
{
    out = ImportCounts{};
    if (!IrrigationService::validateEnvelope(json, len, err, errCap))
        return false;

    // Obtém o primeiro client do envelope.
    Slice client{};
    FirstClientCtx fcc{&client};
    IrrigationService::envelopeForEachClient(json, len, &fcc, firstClientCb);
    if (!client.p) {
        if (errCap) snprintf(err, errCap, "sem client");
        return false;
    }

    // Os 4 arrays de configuração ficam dentro do sub-objeto "config" do client
    // (ver buildClientBackup em ServiceBackup.cpp: w.key("config"); w.beginObject(); ...).
    Slice config{};
    if (!IrrigationService::jsonMember(client.p, client.n, "config", config)) {
        // Envelope sem "config" (e.g. versão antiga sem esse wrapper): tratar como 0 itens.
        return true;
    }

    ZCtx   zc{&zones,      &out.zonas};
    PCtx   pc{&sched,      &out.programas};
    ICtx   ic{&interlocks, &out.intertravamentos};
    GCtx   gc{&groups,     &out.grupos};

    importArray(config.p, config.n, "zonas",            &zc, applyZone);
    importArray(config.p, config.n, "programas",        &pc, applyProgram);
    importArray(config.p, config.n, "intertravamentos", &ic, applyInterlock);
    importArray(config.p, config.n, "grupos",           &gc, applyGroup);
    return true;
}

// ── Supressão meteorológica — builders e parsers (fase 11) ───────────────────

// Serializa um decimal com 1 casa fracionária (ex.: -235 → "-23.5").
// Usado para converter latE7/lonE7 divididos por 1e6 em graus com 1 decimal.
// Nota: para lat/lon já em E7 precisamos dividir por 1e7; mas como JsonWriter
// só emite int64, emitimos a representação textual via raw().
static void fmtE7ToDecimal(char *out, size_t cap, int32_t e7)
{
    // ex.: e7=-235000000 → "-23.5000000"
    // Emitimos com 4 casas (precisão suficiente para UI; task 13 usa o valor).
    int32_t intPart = e7 / 10000000;
    int32_t fracPart = e7 % 10000000;
    if (fracPart < 0) fracPart = -fracPart;
    snprintf(out, cap, "%d.%07d", intPart, fracPart);
}

size_t buildWeatherStatus(const WeatherStatusCtx &ctx, char *buf, size_t cap)
{
    if (!ctx.cfg || !ctx.cache || !ctx.rules) return 0;
    const WeatherCache &c = *ctx.cache;
    const WeatherConfig &cfg = *ctx.cfg;
    bool fresh = WeatherEngine::cacheFresh(c, ctx.nowEpoch, cfg.staleTtlH);

    JsonWriter w(buf, cap);
    w.beginObject();
    w.keyBool("enabled", cfg.enabled != 0);
    // lat/lon: emite como número decimal (raw) para não quebrar JS parseFloat
    { char t[20]; fmtE7ToDecimal(t, sizeof(t), cfg.latE7); w.key("lat"); w.raw(t); }
    { char t[20]; fmtE7ToDecimal(t, sizeof(t), cfg.lonE7); w.key("lon"); w.raw(t); }
    w.keyNum("updatedEpoch", (int64_t)c.fetchEpoch);
    w.keyBool("isMock", c.isMock);
    w.keyBool("staUp", ctx.staUp);
    w.keyStr("location", ctx.location ? ctx.location : "");
    // metrics — espelha card do mockup
    w.key("metrics");
    w.beginObject();
    w.keyNum("chuvaPrevista12hCenti", c.chuvaPrevista12hCenti);
    w.keyNum("probChuva", c.probChuvaPct);
    w.keyNum("chuvaAcum24hCenti", c.chuvaAcum24hCenti);
    w.keyNum("umidadeSolo", c.umidadeSoloPct);
    w.keyNum("tempMinCenti", c.tempMinCenti);
    w.keyNum("tempMaxCenti", c.tempMaxCenti);
    w.keyNum("ventoRajadaCenti", c.ventoRajadaCenti);
    w.keyNum("et0Centi", c.et0Centi);
    w.keyNum("tempAtualCenti", c.tempAtualCenti);
    w.keyNum("umidadeRel", c.umidadeRelPct);
    w.endObject();
    // regras — inclui veredito ao vivo para o badge "Suprimindo"
    bool anySup = false;
    w.key("rules");
    w.beginArray();
    for (size_t i = 0; i < ctx.rules->count(); i++) {
        const WeatherRule *r = ctx.rules->ruleAt(i);
        if (!r) break;
        bool trig = WeatherEngine::ruleTriggered(*r, c, ctx.nowEpoch, cfg.staleTtlH);
        if (trig) anySup = true;
        w.beginObject();
        w.keyNum("id", r->id);
        w.keyStr("nome", r->nome);
        w.keyBool("enabled", r->enabled != 0);
        w.keyNum("limiarMmCenti", r->limiarMmCenti);
        w.keyNum("limiarPct", r->limiarPct);
        w.keyStr("mensagem", r->mensagem);
        w.key("zonaIds");
        w.beginArray();
        for (uint8_t z : r->zonaIds) if (z) w.num(z);
        w.endArray();
        w.key("grupoIds");
        w.beginArray();
        for (uint8_t g : r->grupoIds) if (g) w.num(g);
        w.endArray();
        w.keyBool("triggered", trig);
        w.keyBool("fresh", fresh);
        w.keyNum("chuvaAtualCenti", c.chuvaPrevista12hCenti);
        w.keyNum("probAtual", c.probChuvaPct);
        w.endObject();
    }
    w.endArray();
    w.keyBool("anySuppressed", anySup);
    w.endObject();
    return w.done();
}

// Scanner local de número decimal (positivo ou negativo) sem usar strtod.
// Retorna true e define *out (em centi: valor×100 arredondado) se encontrou
// o valor da chave key no JSON plano body[0..n).
// Suporta: inteiros (ex.: 3) e decimais (ex.: 3.5, -23.5000000).
static bool scanDecimalCenti(const char *body, size_t n, const char *key, int32_t &out)
{
    // Reutiliza JsonReader::findValue indiretamente: monta o padrão "key":
    char pat[48];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0) return false;
    size_t patLen = (size_t)pn;
    const char *end = body + n;
    const char *p = body;
    while (p + patLen <= end) {
        if (memcmp(p, pat, patLen) != 0) { p++; continue; }
        const char *q = p + patLen;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != ':') { p = q; continue; }
        q++;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end) return false;
        // lê sinal
        bool neg = false;
        if (*q == '-') { neg = true; q++; }
        else if (*q == '+') { q++; }
        // parte inteira
        int64_t intPart = 0;
        bool anyDigit = false;
        while (q < end && *q >= '0' && *q <= '9') { intPart = intPart * 10 + (*q - '0'); q++; anyDigit = true; }
        if (!anyDigit) return false;
        // parte fracionária (até 2 dígitos para centi)
        int64_t fracCenti = 0;
        if (q < end && *q == '.') {
            q++;
            int places = 0;
            while (q < end && *q >= '0' && *q <= '9' && places < 2) {
                fracCenti = fracCenti * 10 + (*q - '0');
                q++; places++;
            }
            // se só 1 dígito decimal, multiplicar por 10 (ex.: ".5" → 50 centésimos)
            if (places == 1) fracCenti *= 10;
            // ignorar dígitos extras além da 2ª casa
            while (q < end && *q >= '0' && *q <= '9') q++;
        }
        int64_t centi = intPart * 100 + fracCenti;
        out = (int32_t)(neg ? -centi : centi);
        return true;
    }
    return false;
}

// Scanner de número decimal para E7 (para lat/lon).
static bool scanDecimalE7(const char *body, size_t n, const char *key, int32_t &out)
{
    char pat[48];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn <= 0) return false;
    size_t patLen = (size_t)pn;
    const char *end = body + n;
    const char *p = body;
    while (p + patLen <= end) {
        if (memcmp(p, pat, patLen) != 0) { p++; continue; }
        const char *q = p + patLen;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end || *q != ':') { p = q; continue; }
        q++;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q >= end) return false;
        bool neg = false;
        if (*q == '-') { neg = true; q++; }
        else if (*q == '+') { q++; }
        int64_t intPart = 0;
        bool anyDigit = false;
        while (q < end && *q >= '0' && *q <= '9') { intPart = intPart * 10 + (*q - '0'); q++; anyDigit = true; }
        if (!anyDigit) return false;
        // parte fracionária (até 7 dígitos para E7)
        int64_t fracE7 = 0;
        int fracPlaces = 0;
        if (q < end && *q == '.') {
            q++;
            while (q < end && *q >= '0' && *q <= '9' && fracPlaces < 7) {
                fracE7 = fracE7 * 10 + (*q - '0');
                q++; fracPlaces++;
            }
            // preenche zeros à direita até 7 casas
            while (fracPlaces < 7) { fracE7 *= 10; fracPlaces++; }
            // ignora dígitos extras
            while (q < end && *q >= '0' && *q <= '9') q++;
        } else {
            fracE7 = 0;
        }
        int64_t e7 = intPart * 10000000LL + fracE7;
        // arredondamento: ½ ULP
        out = (int32_t)(neg ? -e7 : e7);
        return true;
    }
    return false;
}

// Scanner de array de inteiros (uint8). Localiza "key":[ e extrai os números.
// Reutiliza a mesma técnica de parseInterlockUpsert / parseGroupUpsert.
static uint8_t scanUint8Array(const char *body, size_t n, const char *key,
                              uint8_t *out, uint8_t cap)
{
    char keyStr[48];
    snprintf(keyStr, sizeof(keyStr), "\"%s\"", key);
    const char *sp = strstr(body, keyStr);
    if (!sp) return 0;
    // acha '[' após a chave (pode ser "key": [)
    const char *arr = strchr(sp + strlen(keyStr), '[');
    if (!arr || arr >= body + n) return 0;
    const char *arrEnd = strchr(arr, ']');
    if (!arrEnd || arrEnd >= body + n) return 0;
    uint8_t count = 0;
    const char *o = arr + 1;
    while (o < arrEnd && count < cap) {
        while (o < arrEnd && (*o < '0' || *o > '9')) o++;
        if (o >= arrEnd) break;
        char tok[8]; size_t ti = 0; const char *p2 = o; bool anyD = false;
        while (p2 < arrEnd && *p2 >= '0' && *p2 <= '9' && ti + 1 < sizeof(tok)) {
            tok[ti++] = *p2++; anyD = true;
        }
        if (anyD) {
            tok[ti] = '\0';
            char *tend = nullptr;
            long long zid = strtoll(tok, &tend, 10);
            if (tend != tok && zid > 0) out[count++] = (uint8_t)zid;
        }
        o = p2;
        while (o < arrEnd && *o != ',') o++;
        if (o < arrEnd) o++;
    }
    return count;
}

WeatherRuleParse parseWeatherRule(const char *body, size_t n)
{
    WeatherRuleParse result;
    if (!body || n == 0) { result.err = "corpo vazio"; return result; }

    JsonReader rd(body, n);

    // nome (obrigatório, não-vazio)
    char nome[WeatherRule::NOME_LEN] = {0};
    if (!rd.getStr("nome", nome, sizeof(nome)) || nome[0] == '\0') {
        result.err = "nome vazio/ausente";
        return result;
    }

    // limiarMm (decimal → centi-mm)
    int32_t limiarMmCenti = 0;
    if (!scanDecimalCenti(body, n, "limiarMm", limiarMmCenti)) {
        result.err = "limiarMm ausente/invalido";
        return result;
    }

    // limiarPct (inteiro)
    int64_t limiarPct = 0;
    rd.getInt("limiarPct", limiarPct);

    // enabled (opcional, default true)
    bool enabled = true;
    rd.getBool("enabled", enabled);

    // mensagem (opcional)
    char mensagem[WeatherRule::MSG_LEN] = {0};
    rd.getStr("mensagem", mensagem, sizeof(mensagem));

    // zonaIds e grupoIds (arrays de uint8)
    uint8_t zonaIds[WeatherRule::MAX_ZONE_TARGETS] = {0};
    uint8_t grupoIds[WeatherRule::MAX_GROUP_TARGETS] = {0};
    uint8_t zCount = scanUint8Array(body, n, "zonaIds", zonaIds, WeatherRule::MAX_ZONE_TARGETS);
    uint8_t gCount = scanUint8Array(body, n, "grupoIds", grupoIds, WeatherRule::MAX_GROUP_TARGETS);

    // validação: precisa de ao menos um alvo
    if (zCount == 0 && gCount == 0) {
        result.err = "pelo menos uma zona ou grupo obrigatorio";
        return result;
    }

    // preenche a regra
    WeatherRule &r = result.rule;
    r = WeatherRule{};
    strncpy(r.nome, nome, WeatherRule::NOME_LEN - 1);
    strncpy(r.mensagem, mensagem, WeatherRule::MSG_LEN - 1);
    r.limiarMmCenti = (uint16_t)(limiarMmCenti < 0 ? 0 : limiarMmCenti);
    r.limiarPct = (uint8_t)(limiarPct < 0 ? 0 : (limiarPct > 100 ? 100 : limiarPct));
    r.enabled = enabled ? 1 : 0;
    for (uint8_t i = 0; i < WeatherRule::MAX_ZONE_TARGETS; i++)
        r.zonaIds[i] = i < zCount ? zonaIds[i] : 0;
    for (uint8_t i = 0; i < WeatherRule::MAX_GROUP_TARGETS; i++)
        r.grupoIds[i] = i < gCount ? grupoIds[i] : 0;

    result.ok = true;
    return result;
}

WeatherConfigParse parseWeatherConfig(const char *body, size_t n)
{
    WeatherConfigParse result;
    if (!body || n == 0) { result.err = "corpo vazio"; return result; }

    JsonReader rd(body, n);

    // enabled (bool)
    bool enabled = false;
    bool hasEnabled = rd.getBool("enabled", enabled);
    if (!hasEnabled) {
        // tenta como inteiro (ex.: "enabled":1)
        int64_t en = 0;
        if (rd.getInt("enabled", en)) { enabled = (en != 0); hasEnabled = true; }
    }

    // lat e lon (decimais → E7)
    int32_t latE7 = 0, lonE7 = 0;
    bool hasLat = scanDecimalE7(body, n, "lat", latE7);
    bool hasLon = scanDecimalE7(body, n, "lon", lonE7);

    if (!hasLat || !hasLon) {
        result.err = "lat/lon ausentes";
        return result;
    }

    // validação de faixa
    if (latE7 < -900000000 || latE7 > 900000000) {
        result.err = "lat fora de -90..90";
        return result;
    }
    if (lonE7 < -1800000000 || lonE7 > 1800000000) {
        result.err = "lon fora de -180..180";
        return result;
    }

    result.enabled = enabled ? 1 : 0;
    result.latE7 = latE7;
    result.lonE7 = lonE7;
    result.ok = true;
    return result;
}

} // namespace IrrigationWeb
