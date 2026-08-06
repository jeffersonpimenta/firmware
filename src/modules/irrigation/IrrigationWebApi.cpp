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

} // namespace IrrigationWeb
