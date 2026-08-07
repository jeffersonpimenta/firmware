#include "modules/irrigation/IrrigationWebEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "modules/irrigation/IrrigationEndpointHelpers.h" // sendJson/sendParseErrors/readBody
#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/IrrigationSettings.h" // migrateIrrigationSettings (hStations lê o blob)
#include "modules/irrigation/IrrigationWebApi.h"

#include <Arduino.h> // millis()
#include <cstdlib>   // malloc/free (hAudit aloca ~16 KB no heap)
#include <string>    // std::string (getQueryParameter)

// Mesma sequência de include do esp32_https_server usada por ContentHandler.cpp:
// "#undef str" antes dos headers do servidor (workaround gcc bug 57824).
#undef str
#include <ResourceNode.hpp>

using namespace httpsserver;
using namespace IrrigationWeb;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool gwReady()
{
    return irrigationModule && irrigationModule->gwIsGateway();
}

// ---------------------------------------------------------------------------
// GET handlers
// ---------------------------------------------------------------------------

static void hAlerts(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char buf[3072];
    if (!irrigationModule->gwBuildAlerts(buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hOverview(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    const IrrigationGateway &g = irrigationModule->gwState();
    OverviewCtx c = {};
    c.hasRtc = irrigationModule->gwHasRtc();
    c.stationCount = (uint16_t)g.stations.count();
    c.running = g.scheduler.running() ? 1 : 0;
    c.runningZoneId = g.scheduler.currentZone();
    c.alertCount = irrigationModule->gwUnackedAlertCount(); // pendentes (não o total do ring)
    // Fase 9: estado de pareamento pendente (§6) — announce recebido com a janela fechada.
    c.pairingPending = irrigationModule->gwPairingPending();
    c.pairingNodeId = irrigationModule->gwPairingNode();
    c.pairingSecondsLeft = irrigationModule->gwPairingSecondsLeft();
    char buf[512];
    if (!buildOverview(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hStations(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    const IrrigationGateway &g = irrigationModule->gwState();
    const uint32_t nowMs = millis();

    StationView views[StationRegistry::MAX];
    size_t n = 0;
    for (size_t i = 0; i < g.stations.count() && n < StationRegistry::MAX; i++) {
        const StationEntry *e = g.stations.nodeAt(i);
        if (!e)
            continue;
        const StationTelemetry *tel = g.telemetry.byNode(e->node);

        uint32_t lastHeard = g.monitor.lastHeardMs(e->node);
        uint32_t secsSinceHeard = (lastHeard && nowMs >= lastHeard) ? (nowMs - lastHeard) / 1000 : 0;
        bool silent = (uint64_t)secsSinceHeard * 1000 > (uint64_t)e->silencioAlertaMin * 60000;

        StationView &v = views[n++];
        v.node = e->node;
        v.name = e->name;
        v.sync = computeSync(e->desiredEpoch, tel ? tel->configEpoch : 0, silent);
        v.secsSinceHeard = secsSinceHeard;
        v.vbatCentiV = tel ? tel->vbatCentiV : 0;
        v.vpanelCentiV = tel ? tel->vpanelCentiV : 0;
        v.snrQuarterDb = tel ? tel->snrQuarterDb : 0;
        v.rebootCount = tel ? tel->rebootCount : 0;
        v.flags = tel ? tel->flags : 0;
        v.rssiDbm = tel ? tel->rssiDbm : 0;
        v.lat = e->lat;
        v.lon = e->lon;

        // Fase 9: config desejada (heartbeat + limiares) e saídas físicas, do blob adotado.
        IrrigationSettings cfg;
        if (e->desiredEpoch != 0 && migrateIrrigationSettings(e->blob, sizeof(e->blob), cfg)) {
            v.hbMinutes = cfg.hbMinutes;
            v.vbatAvisoCentiV = cfg.vbatAvisoCentiV;
            v.vbatCriticaCentiV = cfg.vbatCriticaCentiV;
            uint8_t oc = 0;
            for (uint8_t k = 0; k < IrrigationSettings::MAX_VALVES && oc < StationView::MAX_OUTPUTS; k++)
                if (cfg.pinsHbridgeA[k] >= 0) {
                    v.outputs[oc].tipo = 0;
                    v.outputs[oc].index = k;
                    oc++;
                }
            for (uint8_t k = 0; k < IrrigationSettings::MAX_GPO && oc < StationView::MAX_OUTPUTS; k++)
                if (cfg.pinsGpo[k] >= 0) {
                    v.outputs[oc].tipo = 1;
                    v.outputs[oc].index = k;
                    oc++;
                }
            v.outputCount = oc;
        }
    }

    char buf[8192]; // Fase 9: campos extras (rssi/hb/limiares/outputs) por estação
    if (!buildStations(views, n, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// POST /api/irrigation/stations/config — edita heartbeat/limiares/coords + re-push §5.4
static void hStationsConfig(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    StationConfigReq r;
    ParseResult pr = parseStationConfig(body, nb, r);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplyStationConfig(r)) {
        sendJson(res, "{\"errors\":[\"estação sem config adotada\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// POST /api/irrigation/stations/delete — remove estação (trava se há zonas vinculadas)
static void hStationsDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    uint32_t node = 0;
    ParseResult pr = parseStationDelete(body, nb, node);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    int deps = irrigationModule->gwCountZonesForNode(node);
    if (deps > 0) {
        char out[96];
        snprintf(out, sizeof(out), "{\"errors\":[\"%d zona(s) vinculada(s) — mova-as primeiro\"]}", deps);
        sendJson(res, out, 400);
        return;
    }
    if (!irrigationModule->gwRemoveStation(node)) {
        sendJson(res, "{\"errors\":[\"estação inexistente\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// POST /api/irrigation/stations/pulse — teste de pulso por saída física
static void hStationsPulse(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    StationPulseReq r;
    ParseResult pr = parseStationPulse(body, nb, r);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwStationPulse(r)) {
        sendJson(res, "{\"errors\":[\"saída inexistente\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

static void hZonesGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char buf[3072];
    if (!buildZones(irrigationModule->gwState().zones, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hProgramsGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char buf[3072];
    if (!buildPrograms(irrigationModule->gwState().scheduler, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// ---------------------------------------------------------------------------
// POST handlers (parse -> apply -> responder)
// ---------------------------------------------------------------------------

static void hZonesPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[512];
    size_t nb = readBody(req, body, sizeof(body));
    Zone z;
    ParseResult pr = parseZoneUpsert(body, nb, z);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplyZoneUpsert(z)) {
        sendJson(res, "{\"errors\":[\"tabela cheia\"]}", 400);
        return;
    }
    char out[3072];
    if (!buildZones(irrigationModule->gwState().zones, out, sizeof(out))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, out);
}

static void hZonesDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0;
    ParseResult pr = parseZoneDelete(body, nb, id);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplyZoneDelete(id)) {
        sendJson(res, "{\"errors\":[\"zona inexistente\"]}", 400);
        return;
    }
    char out[3072];
    if (!buildZones(irrigationModule->gwState().zones, out, sizeof(out))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, out);
}

static void hProgramsPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[1024];
    size_t nb = readBody(req, body, sizeof(body));
    Program p;
    ParseResult pr = parseProgramUpsert(body, nb, p);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplyProgramUpsert(p)) {
        sendJson(res, "{\"errors\":[\"tabela cheia\"]}", 400);
        return;
    }
    char out[3072];
    if (!buildPrograms(irrigationModule->gwState().scheduler, out, sizeof(out))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, out);
}

static void hProgramsToggle(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0;
    bool enabled = false;
    ParseResult pr = parseProgramToggle(body, nb, id, enabled);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplyProgramToggle(id, enabled)) {
        sendJson(res, "{\"errors\":[\"programa inexistente\"]}", 400);
        return;
    }
    char out[3072];
    if (!buildPrograms(irrigationModule->gwState().scheduler, out, sizeof(out))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, out);
}

static void hProgramsDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0;
    ParseResult pr = parseProgramDelete(body, nb, id);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplyProgramDelete(id)) {
        sendJson(res, "{\"errors\":[\"programa inexistente\"]}", 400);
        return;
    }
    char out[3072];
    if (!buildPrograms(irrigationModule->gwState().scheduler, out, sizeof(out))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, out);
}

static void hCommand(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[512];
    size_t nb = readBody(req, body, sizeof(body));
    WebCommand cmd;
    ParseResult pr = parseCommand(body, nb, cmd);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwRunCommand(cmd)) {
        sendJson(res, "{\"errors\":[\"comando rejeitado\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Fase 6b Task 18: intertravamentos, sensores, log de auditoria, manutenção
// NOTA: estes handlers ficam dentro do mesmo guard MESHTASTIC_EXCLUDE_WEBSERVER
// que os demais endpoints — mas o bloco abaixo só compila em ARCH_ESP32 com
// webserver habilitado (condição idêntica a ContentHandler.cpp). As implementações
// são CI-only e não exercitadas pelo native test suite.
// ---------------------------------------------------------------------------

// GET /api/irrigation/interlocks — lista regras de intertravamento
static void hInterlocksGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char buf[3072];
    size_t n = buildInterlocks(irrigationModule->gwState().interlocks, buf, sizeof(buf));
    if (!n) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// POST /api/irrigation/interlocks — upsert de uma regra
static void hInterlocksPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[512];
    size_t nb = readBody(req, body, sizeof(body));
    InterlockRule rule;
    ParseResult pr = parseInterlockUpsert(body, nb, rule);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplyInterlockUpsert(rule)) {
        sendJson(res, "{\"errors\":[\"tabela cheia\"]}", 400);
        return;
    }
    char out[3072];
    size_t n = buildInterlocks(irrigationModule->gwState().interlocks, out, sizeof(out));
    if (!n) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, out);
}

// POST /api/irrigation/interlocks/delete — remove uma regra por id
static void hInterlocksDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0;
    ParseResult pr = parseInterlockDelete(body, nb, id);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplyInterlockDelete(id)) {
        sendJson(res, "{\"errors\":[\"regra inexistente\"]}", 400);
        return;
    }
    char out[3072];
    size_t n = buildInterlocks(irrigationModule->gwState().interlocks, out, sizeof(out));
    if (!n) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, out);
}

// GET /api/irrigation/sensors — leitura de sensores de todas as estações
static void hSensorsGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    const IrrigationGateway &g = irrigationModule->gwState();

    GwStationSensors views[StationTelemetryCache::MAX];
    size_t nViews = 0;

    for (size_t i = 0; i < StationTelemetryCache::MAX && nViews < StationTelemetryCache::MAX; i++) {
        const StationTelemetry *t = g.telemetry.entryAt(i);
        if (!t || !t->node)
            continue;

        GwStationSensors &v = views[nViews++];
        v.node = t->node;
        v.tamper = t->tamper;

        // Nome da estação via StationRegistry; "" se não registrada
        const StationEntry *st = g.stations.byNode(t->node);
        v.stationName = (st && st->name[0]) ? st->name : "";

        // Monta itens de sensor
        v.count = 0;
        for (uint8_t k = 0; k < t->sensorCount && k < IrrigationProto::HB_MAX_SENSORS; k++) {
            GwSensorItem &item = v.itens[v.count++];
            item.idx = t->sensors[k].id;
            item.tipo = t->sensors[k].tipo;
            item.valueCenti = t->sensors[k].valueCenti;
            const char *nm = g.sensorNames.get(t->node, t->sensors[k].id);
            item.name = nm ? nm : "";
        }
    }

    char buf[4096];
    size_t n = buildSensorsGateway(views, nViews, buf, sizeof(buf));
    if (!n) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// POST /api/irrigation/sensors/name — define nome de um sensor de uma estação
static void hSensorsName(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    uint32_t node = 0;
    uint8_t idx = 0;
    char name[17]; // max 16 chars + NUL
    ParseResult pr = parseSensorName(body, nb, node, idx, name, sizeof(name));
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwApplySensorName(node, idx, name)) {
        sendJson(res, "{\"errors\":[\"tabela de nomes cheia\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// GET /api/irrigation/audit — log de auditoria flash do gateway (JSON ou CSV)
// Parâmetros de query: fmt=json|csv  n=<maxRecords> (padrão 500)
static void hAudit(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    // Lê parâmetros de query: fmt e n
    ResourceParameters *params = req->getParams();
    std::string fmtVal;
    bool wantCsv = false;
    if (params->getQueryParameter("fmt", fmtVal) && fmtVal == "csv")
        wantCsv = true;

    size_t maxRecords = 500;
    std::string nVal;
    if (params->getQueryParameter("n", nVal)) {
        int parsed = 0;
        for (char c : nVal)
            if (c >= '0' && c <= '9')
                parsed = parsed * 10 + (c - '0');
        if (parsed > 0)
            maxRecords = (size_t)parsed;
    }

    // Log de ~16 KB no heap para não explodir o stack da task HTTP.
    // Espelha o idioma de hLog em IrrigationPortalEndpoints.cpp.
    const size_t cap = 16384;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }

    size_t n = 0;
    if (wantCsv)
        n = irrigationModule->auditFlashRef().toCsv(buf, cap, maxRecords);  // CI-only
    else
        n = irrigationModule->auditFlashRef().toJson(buf, cap, maxRecords); // CI-only

    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }

    res->setStatusCode(200);
    res->setHeader("Content-Type", wantCsv ? "text/csv" : "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->print(buf);
    free(buf);
}

// POST /api/irrigation/maint — abre/fecha janela de manutenção de tamper numa estação
static void hMaint(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    uint32_t node = 0;
    uint16_t minutes = 0;
    ParseResult pr = parseMaintWindow(body, nb, node, minutes);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    irrigationModule->gwOpenMaintWindow(node, minutes);
    sendJson(res, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Fase 7b: grupos hidráulicos
// ---------------------------------------------------------------------------

// GET /api/irrigation/groups — lista de config dos grupos
static void hGroupsGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    char buf[2048];
    size_t n = buildGroups(irrigationModule->gwState().groups, buf, sizeof(buf));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// GET /api/irrigation/groups/status — status ao vivo por grupo
static void hGroupsStatus(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    const IrrigationGateway &g = irrigationModule->gwState();

    GroupStatusView views[HydraulicGroupTable::MAX];
    size_t nv = 0;
    for (size_t i = 0; i < g.groups.count() && nv < HydraulicGroupTable::MAX; i++) {
        const HydraulicGroup *grp = g.groups.groupAt(i);
        if (!grp) continue;
        GroupStatusView &v = views[nv++];
        v.id = grp->id;
        v.name = grp->name;
        v.state = (uint8_t)g.groupEngine.stateOf(grp->id);
        v.pump = g.groupEngine.pumpOn(grp->id);
        v.curZone = g.groupEngine.currentZone(grp->id);
        v.openCount = g.groupEngine.openConfirmedCount(grp->id);
    }
    char buf[2048];
    size_t n = buildGroupsStatus(views, nv, buf, sizeof(buf));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// POST /api/irrigation/groups — upsert (id=0 => servidor aloca)
static void hGroupsPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[768];
    size_t nb = readBody(req, body, sizeof(body));
    HydraulicGroup grp;
    ParseResult pr = parseGroupUpsert(body, nb, grp);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    char err[48] = {0};
    if (!irrigationModule->gwApplyGroupUpsert(grp, err, sizeof(err))) {
        char out[128];
        JsonWriter w(out, sizeof(out));
        w.beginObject(); w.key("errors"); w.beginArray(); w.str(err[0] ? err : "erro"); w.endArray(); w.endObject();
        w.done();
        sendJson(res, out, 400);
        return;
    }
    char out[2048];
    size_t n = buildGroups(irrigationModule->gwState().groups, out, sizeof(out));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, out);
}

// POST /api/irrigation/groups/delete — remove por id
static void hGroupsDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0;
    ParseResult pr = parseGroupDelete(body, nb, id);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->gwApplyGroupDelete(id)) {
        sendJson(res, "{\"errors\":[\"grupo inexistente\"]}", 400);
        return;
    }
    char out[2048];
    size_t n = buildGroups(irrigationModule->gwState().groups, out, sizeof(out));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, out);
}

// POST /api/irrigation/groups/command — controle manual do grupo
static void hGroupsCommand(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0; bool open = false; uint16_t dur = 0;
    ParseResult pr = parseGroupCommand(body, nb, id, open, dur);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->gwRunGroupCommand(id, open, dur)) {
        sendJson(res, "{\"errors\":[\"grupo inexistente\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Fase 8b (Task 7): controle de nível por boia
// ---------------------------------------------------------------------------

// GET /api/irrigation/levels — lista de regras de controle de nível
static void hLevelsGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    char buf[1024];
    size_t n = buildLevelControls(irrigationModule->gwState().levels, buf, sizeof(buf));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// POST /api/irrigation/levels — upsert (id=0 => servidor aloca)
static void hLevelsPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[512];
    size_t nb = readBody(req, body, sizeof(body));
    LevelRule r;
    ParseResult pr = parseLevelUpsert(body, nb, r);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    char err[48] = {0};
    if (!irrigationModule->gwApplyLevelUpsert(r, err, sizeof(err))) {
        char out[128];
        JsonWriter w(out, sizeof(out));
        w.beginObject(); w.key("errors"); w.beginArray(); w.str(err[0] ? err : "erro"); w.endArray(); w.endObject();
        w.done();
        sendJson(res, out, 400);
        return;
    }
    char out[1024];
    size_t n = buildLevelControls(irrigationModule->gwState().levels, out, sizeof(out));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, out);
}

// POST /api/irrigation/levels/delete — remove por id
static void hLevelsDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    uint8_t id = 0;
    ParseResult pr = parseLevelDelete(body, nb, id);
    if (!pr.ok) { sendParseErrors(res, pr); return; }
    if (!irrigationModule->gwApplyLevelDelete(id)) {
        sendJson(res, "{\"errors\":[\"regra inexistente\"]}", 400);
        return;
    }
    char out[1024];
    size_t n = buildLevelControls(irrigationModule->gwState().levels, out, sizeof(out));
    if (!n) { res->setStatusCode(500); return; }
    sendJson(res, out);
}

// ---------------------------------------------------------------------------
// Modo Espelhamento UI (Fase X): 4 endpoints CI-only.
// ---------------------------------------------------------------------------

// GET /api/irrigation/mirror — estado ao vivo do modo espelho
static void hMirror(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char buf[1024];
    size_t n = irrigationModule->gwBuildMirror(buf, sizeof(buf));
    if (!n) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// POST /api/irrigation/mirror — habilita/desabilita o modo espelho  {enabled:bool}
static void hMirrorToggle(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    bool en = false;
    ParseResult pr = parseMirrorToggle(body, nb, en);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    irrigationModule->gwSetMirrorEnabled(en);
    char buf[1024];
    size_t n = irrigationModule->gwBuildMirror(buf, sizeof(buf));
    if (!n) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// POST /api/irrigation/mirror/mapping — associa porta→zona  {input,zoneId,invertido?,habilitado?}
static void hMirrorMapping(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    int8_t in = 0;
    uint8_t zid = 0;
    bool inv = false, hab = true;
    ParseResult pr = parseMirrorMapping(body, nb, in, zid, inv, hab);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    char err[48] = {0};
    if (!irrigationModule->gwApplyMirrorMapping(in, zid, inv, hab, err, sizeof(err))) {
        char out[128];
        JsonWriter w(out, sizeof(out));
        w.beginObject();
        w.key("errors");
        w.beginArray();
        w.str(err[0] ? err : "erro");
        w.endArray();
        w.endObject();
        w.done();
        sendJson(res, out, 400);
        return;
    }
    char buf[1024];
    size_t n = irrigationModule->gwBuildMirror(buf, sizeof(buf));
    if (!n) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// POST /api/irrigation/mirror/mapping/delete — remove mapeamento de uma porta  {input:0..3}
static void hMirrorMappingDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    // Só precisa de "input": usa o JsonReader diretamente (parseMirrorMapping exige zoneId também).
    JsonReader rd(body, nb);
    int64_t in = -1;
    if (!rd.getInt("input", in) || in < 0 || in > 3) {
        sendJson(res, "{\"errors\":[\"input fora de 0..3\"]}", 400);
        return;
    }
    if (!irrigationModule->gwDeleteMirrorMapping((int8_t)in)) {
        sendJson(res, "{\"errors\":[\"porta sem zona associada\"]}", 400);
        return;
    }
    char buf[1024];
    size_t n = irrigationModule->gwBuildMirror(buf, sizeof(buf));
    if (!n) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// POST /api/irrigation/import — restaura as 4 tabelas de config de um envelope de backup (§5.5).
// NÃO toca PSK/canal; NÃO reinicializa; NÃO bumpa epoch das estações. CI-only.
static void hImport(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    const size_t cap = 8192; // mesma capacidade que hExport/gwBuildBackup
    char *body = (char *)malloc(cap);
    if (!body) {
        res->setStatusCode(500);
        return;
    }
    size_t nb = readBody(req, body, cap);
    char resp[128];
    bool ok = irrigationModule->gwImportTables(body, nb, resp, sizeof(resp));
    free(body);
    sendJson(res, resp, ok ? 200 : 400);
}

// GET /api/irrigation/export — backup §5.5 completo (PSK + tabelas) num envelope
// multi-cliente, para o cofre do device SERVICO (Fase 8b §11.7).
static void hExport(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    const size_t cap = 8192;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = irrigationModule->gwBuildBackup(buf, cap); // CI-only
    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }
    buf[n < cap ? n : cap - 1] = 0;
    res->setStatusCode(200);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Content-Disposition", "attachment; filename=\"irrigacao-backup.json\"");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->print(buf);
    free(buf);
}

// GET /api/irrigation/survey — pontos de cobertura registrados (§8.5). GATEWAY-only.
static void hSurvey(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    const size_t cap = 8192;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = irrigationModule->buildSurveyLog(buf, cap); // CI-only
    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
    free(buf);
}

// POST /api/irrigation/survey/clear — zera o log de cobertura.
static void hSurveyClear(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    irrigationModule->clearSurveyLog();
    sendJson(res, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Fase 8b: Horário (relógio do gateway)
// ---------------------------------------------------------------------------

// GET /api/irrigation/time — estado do relógio do gateway
static void hTime(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char buf[512];
    if (!irrigationModule->gwBuildTimeStatus(buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// POST /api/irrigation/time — set manual { epoch }
static void hTimeSet(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    uint32_t epoch = 0;
    ParseResult pr = parseTimeSet(body, nb, epoch);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwSetManualTime(epoch)) {
        sendJson(res, "{\"ok\":false,\"reason\":\"epoch inválido\"}", 400);
        return;
    }
    char buf[512];
    irrigationModule->gwBuildTimeStatus(buf, sizeof(buf));
    sendJson(res, buf);
}

// POST /api/irrigation/timezone — { tz } (preset POSIX)
static void hTimezone(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    char tz[40] = {0};
    ParseResult pr = parseTimezone(body, nb, tz, sizeof(tz));
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->gwSetTimezone(tz)) {
        sendJson(res, "{\"ok\":false,\"reason\":\"fuso desconhecido\"}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// POST /api/irrigation/time/sync — dispara NTP agora (só com WiFi STA)
static void hTimeSync(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) {
        res->setStatusCode(404);
        return;
    }
    if (!irrigationModule->gwSyncNtpNow()) {
        sendJson(res, "{\"ok\":false,\"reason\":\"sem WiFi\"}", 409);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Supressão meteorológica (Task 12)
// ---------------------------------------------------------------------------

// GET /api/irrigation/weather — status completo (config + cache + regras)
static void hWeatherGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    WeatherStatusCtx c = {};
    c.cfg      = &irrigationModule->gwWeatherConfig();
    c.rules    = &irrigationModule->gwWeatherRules();
    c.cache    = &irrigationModule->gwWeatherCache();
    c.nowEpoch = irrigationModule->gwLocalSecs();
    c.staUp    = irrigationModule->gwStaConnected();
    c.location = irrigationModule->gwNodeLabel();
    char buf[3072];
    if (!buildWeatherStatus(c, buf, sizeof(buf))) { res->setStatusCode(500); return; }
    sendJson(res, buf);
}

// POST /api/irrigation/weather/config — habilita/desabilita + coordenadas
static void hWeatherConfigPost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    WeatherConfigParse p = parseWeatherConfig(body, nb);
    if (!p.ok) { sendJson(res, "{\"ok\":false,\"reason\":\"coordenadas inválidas\"}", 400); return; }
    bool ok = irrigationModule->gwWeatherSetConfig(p.enabled, p.latE7, p.lonE7);
    sendJson(res, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// POST /api/irrigation/weather/rule — upsert de regra de supressão
static void hWeatherRulePost(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[1024];
    size_t nb = readBody(req, body, sizeof(body));
    WeatherRuleParse p = parseWeatherRule(body, nb);
    if (!p.ok) {
        char e[128];
        snprintf(e, sizeof(e), "{\"ok\":false,\"errors\":[\"%s\"]}", p.err);
        sendJson(res, e, 400);
        return;
    }
    uint8_t id = irrigationModule->gwWeatherUpsertRule(p.rule);
    if (!id) { sendJson(res, "{\"ok\":false,\"errors\":[\"tabela cheia\"]}", 400); return; }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"id\":%u}", id);
    sendJson(res, out);
}

// POST /api/irrigation/weather/rule/delete — remove regra por id
static void hWeatherRuleDelete(HTTPRequest *req, HTTPResponse *res)
{
    if (!gwReady()) { res->setStatusCode(404); return; }
    char body[64];
    size_t nb = readBody(req, body, sizeof(body));
    (void)nb;
    int id = 0;
    const char *p = strstr(body, "\"id\"");
    if (p) id = atoi(p + 4 + strspn(p + 4, "\": "));
    bool ok = id && irrigationModule->gwWeatherDeleteRule((uint8_t)id);
    sendJson(res, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// POST /api/irrigation/weather/refresh — dispara poll imediato Open-Meteo
static void hWeatherRefresh(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!gwReady()) { res->setStatusCode(404); return; }
    bool ok = irrigationModule->gwWeatherRefresh();
    sendJson(res, ok ? "{\"ok\":true,\"staUp\":true}" : "{\"ok\":false,\"reason\":\"sem WiFi\"}");
}

// ---------------------------------------------------------------------------
// Registro
// ---------------------------------------------------------------------------

void registerIrrigationHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/irrigation/overview", "GET", &hOverview));
    server->registerNode(new ResourceNode("/api/irrigation/alerts", "GET", &hAlerts));
    server->registerNode(new ResourceNode("/api/irrigation/stations", "GET", &hStations));
    server->registerNode(new ResourceNode("/api/irrigation/stations/config", "POST", &hStationsConfig));
    server->registerNode(new ResourceNode("/api/irrigation/stations/delete", "POST", &hStationsDelete));
    server->registerNode(new ResourceNode("/api/irrigation/stations/pulse", "POST", &hStationsPulse));
    server->registerNode(new ResourceNode("/api/irrigation/zones", "GET", &hZonesGet));
    server->registerNode(new ResourceNode("/api/irrigation/zones", "POST", &hZonesPost));
    server->registerNode(new ResourceNode("/api/irrigation/zones/delete", "POST", &hZonesDelete));
    server->registerNode(new ResourceNode("/api/irrigation/programs", "GET", &hProgramsGet));
    server->registerNode(new ResourceNode("/api/irrigation/programs", "POST", &hProgramsPost));
    server->registerNode(new ResourceNode("/api/irrigation/programs/toggle", "POST", &hProgramsToggle));
    server->registerNode(new ResourceNode("/api/irrigation/programs/delete", "POST", &hProgramsDelete));
    server->registerNode(new ResourceNode("/api/irrigation/command", "POST", &hCommand));
    // Fase 6b Task 18: intertravamentos, sensores, log de auditoria, manutenção
    server->registerNode(new ResourceNode("/api/irrigation/interlocks", "GET", &hInterlocksGet));
    server->registerNode(new ResourceNode("/api/irrigation/interlocks", "POST", &hInterlocksPost));
    server->registerNode(new ResourceNode("/api/irrigation/interlocks/delete", "POST", &hInterlocksDelete));
    server->registerNode(new ResourceNode("/api/irrigation/sensors", "GET", &hSensorsGet));
    server->registerNode(new ResourceNode("/api/irrigation/sensors/name", "POST", &hSensorsName));
    server->registerNode(new ResourceNode("/api/irrigation/audit", "GET", &hAudit));
    server->registerNode(new ResourceNode("/api/irrigation/maint", "POST", &hMaint));
    // Fase 7b: grupos hidráulicos
    server->registerNode(new ResourceNode("/api/irrigation/groups", "GET", &hGroupsGet));
    server->registerNode(new ResourceNode("/api/irrigation/groups/status", "GET", &hGroupsStatus));
    server->registerNode(new ResourceNode("/api/irrigation/groups", "POST", &hGroupsPost));
    server->registerNode(new ResourceNode("/api/irrigation/groups/delete", "POST", &hGroupsDelete));
    server->registerNode(new ResourceNode("/api/irrigation/groups/command", "POST", &hGroupsCommand));
    // Fase 8b (Task 7): controle de nível por boia
    server->registerNode(new ResourceNode("/api/irrigation/levels", "GET", &hLevelsGet));
    server->registerNode(new ResourceNode("/api/irrigation/levels", "POST", &hLevelsPost));
    server->registerNode(new ResourceNode("/api/irrigation/levels/delete", "POST", &hLevelsDelete));
    // Modo Espelhamento UI (Fase X): estado/toggle/mapeamento/delete
    server->registerNode(new ResourceNode("/api/irrigation/mirror", "GET", &hMirror));
    server->registerNode(new ResourceNode("/api/irrigation/mirror", "POST", &hMirrorToggle));
    server->registerNode(new ResourceNode("/api/irrigation/mirror/mapping", "POST", &hMirrorMapping));
    server->registerNode(new ResourceNode("/api/irrigation/mirror/mapping/delete", "POST", &hMirrorMappingDelete));
    // Sistema restore: importa tabelas de config de um envelope de backup (NÃO toca PSK). CI-only.
    server->registerNode(new ResourceNode("/api/irrigation/import", "POST", &hImport));
    // Fase 8b: export §5.5 completo (PSK + tabelas) p/ o cofre do device SERVICO
    server->registerNode(new ResourceNode("/api/irrigation/export", "GET", &hExport));
    // Fase 8d: site survey (§8.5)
    server->registerNode(new ResourceNode("/api/irrigation/survey", "GET", &hSurvey));
    server->registerNode(new ResourceNode("/api/irrigation/survey/clear", "POST", &hSurveyClear));
    // Fase 8b: Horário (relógio do gateway)
    server->registerNode(new ResourceNode("/api/irrigation/time", "GET", &hTime));
    server->registerNode(new ResourceNode("/api/irrigation/time", "POST", &hTimeSet));
    server->registerNode(new ResourceNode("/api/irrigation/timezone", "POST", &hTimezone));
    server->registerNode(new ResourceNode("/api/irrigation/time/sync", "POST", &hTimeSync));
    // Supressão meteorológica (Task 12)
    server->registerNode(new ResourceNode("/api/irrigation/weather", "GET", &hWeatherGet));
    server->registerNode(new ResourceNode("/api/irrigation/weather/config", "POST", &hWeatherConfigPost));
    server->registerNode(new ResourceNode("/api/irrigation/weather/rule", "POST", &hWeatherRulePost));
    server->registerNode(new ResourceNode("/api/irrigation/weather/rule/delete", "POST", &hWeatherRuleDelete));
    server->registerNode(new ResourceNode("/api/irrigation/weather/refresh", "POST", &hWeatherRefresh));
}

#endif
