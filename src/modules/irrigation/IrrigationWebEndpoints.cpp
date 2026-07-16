#include "modules/irrigation/IrrigationWebEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/IrrigationWebApi.h"

#include <Arduino.h> // millis()

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
    c.alertCount = (uint16_t)g.alerts.count();
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
// Registro
// ---------------------------------------------------------------------------

void registerIrrigationHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/irrigation/overview", "GET", &hOverview));
    server->registerNode(new ResourceNode("/api/irrigation/stations", "GET", &hStations));
    server->registerNode(new ResourceNode("/api/irrigation/zones", "GET", &hZonesGet));
    server->registerNode(new ResourceNode("/api/irrigation/zones", "POST", &hZonesPost));
    server->registerNode(new ResourceNode("/api/irrigation/zones/delete", "POST", &hZonesDelete));
    server->registerNode(new ResourceNode("/api/irrigation/programs", "GET", &hProgramsGet));
    server->registerNode(new ResourceNode("/api/irrigation/programs", "POST", &hProgramsPost));
    server->registerNode(new ResourceNode("/api/irrigation/programs/toggle", "POST", &hProgramsToggle));
    server->registerNode(new ResourceNode("/api/irrigation/programs/delete", "POST", &hProgramsDelete));
    server->registerNode(new ResourceNode("/api/irrigation/command", "POST", &hCommand));
}

#endif
