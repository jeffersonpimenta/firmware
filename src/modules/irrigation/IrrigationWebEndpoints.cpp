#include "modules/irrigation/IrrigationWebEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/IrrigationWebApi.h"

#include <Arduino.h> // millis()
#include <cstdlib>   // malloc/free (hAudit aloca ~16 KB no heap)
#include <string>    // std::string (getQueryParameter)

// Mesma sequência de include do esp32_https_server usada por ContentHandler.cpp:
// "#undef str" antes dos headers do servidor (workaround gcc bug 57824).
#undef str
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
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

static void sendJson(HTTPResponse *res, const char *body, int status = 200)
{
    res->setStatusCode(status);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->print(body);
}

// Responde 400 com {"errors":[...]} a partir de um ParseResult que falhou.
static void sendParseErrors(HTTPResponse *res, const ParseResult &pr)
{
    char err[256];
    JsonWriter w(err, sizeof(err));
    w.beginObject();
    w.key("errors");
    w.beginArray();
    for (uint8_t i = 0; i < pr.errorCount; i++)
        w.str(pr.errors[i].msg);
    w.endArray();
    w.endObject();
    if (w.done() == 0) {
        // fallback caso o buffer estoure
        sendJson(res, "{\"errors\":[\"erro\"]}", 400);
        return;
    }
    sendJson(res, err, 400);
}

// Lê o corpo da requisição. esp32_https_server expõe HTTPRequest::readBytes(byte*, size_t)
// que devolve o número de bytes lidos (mesmo idioma de handleAPIv1ToRadio em ContentHandler.cpp).
static size_t readBody(HTTPRequest *req, char *buf, size_t cap)
{
    if (cap == 0)
        return 0;
    size_t n = req->readBytes(reinterpret_cast<byte *>(buf), cap - 1);
    if (n >= cap)
        n = cap - 1;
    buf[n] = '\0';
    return n;
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
    // pairing* ficam nos defaults: o estado de pareamento não é exposto por gwState().
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
        v.lat = e->lat;
        v.lon = e->lon;
    }

    char buf[4096];
    if (!buildStations(views, n, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
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
// Registro
// ---------------------------------------------------------------------------

void registerIrrigationHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/irrigation/overview", "GET", &hOverview));
    server->registerNode(new ResourceNode("/api/irrigation/alerts", "GET", &hAlerts));
    server->registerNode(new ResourceNode("/api/irrigation/stations", "GET", &hStations));
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
    // Fase 8b: export §5.5 completo (PSK + tabelas) p/ o cofre do device SERVICO
    server->registerNode(new ResourceNode("/api/irrigation/export", "GET", &hExport));
    // Fase 8d: site survey (§8.5)
    server->registerNode(new ResourceNode("/api/irrigation/survey", "GET", &hSurvey));
    server->registerNode(new ResourceNode("/api/irrigation/survey/clear", "POST", &hSurveyClear));
}

#endif
