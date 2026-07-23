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
    int64_t zoneId = 0, dur = 0, node = 0;
    rd.getInt("zoneId", zoneId);
    rd.getInt("durationS", dur);
    rd.getInt("node", node);
    c.zoneId = (uint8_t)zoneId;
    c.node = (uint32_t)node;
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

} // namespace IrrigationWeb
