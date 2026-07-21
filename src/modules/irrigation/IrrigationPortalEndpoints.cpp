#include "modules/irrigation/IrrigationPortalEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/PortalApi.h"

#include <Arduino.h>

#undef str
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <ResourceNode.hpp>

using namespace httpsserver;
using namespace IrrigationWeb;

static void sendJson(HTTPResponse *res, const char *body, int status = 200)
{
    res->setStatusCode(status);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->print(body);
}

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
        sendJson(res, "{\"errors\":[\"erro\"]}", 400);
        return;
    }
    sendJson(res, err, 400);
}

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

static void hNode(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    NodeStateCtx c = {};
    irrigationModule->portalFillNodeState(c);
    char buf[512];
    if (!buildNodeState(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hNodePulse(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    PortalPulseReq p;
    ParseResult pr = parsePulse(body, nb, p);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalPulse(p)) {
        sendJson(res, "{\"errors\":[\"pulso rejeitado\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

static void hNetRoster(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    const ZoneTable *zones = irrigationModule->gwIsGateway() ? &irrigationModule->gwState().zones : nullptr;
    char buf[2048];
    if (!buildRoster(zones, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hNetCommand(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    NetCommand c;
    ParseResult pr = parseNetCommand(body, nb, c);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalRunNetCommand(c)) {
        sendJson(res, "{\"errors\":[\"comando rejeitado (sem gateway ou zona inexistente)\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

static void hSensors(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    PortalSensorsCtx c = {};
    irrigationModule->portalFillSensors(c);
    char buf[512];
    if (!buildSensors(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hLog(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    // Log cheio (100 registros) ~9,6 KB: buffer no heap, não no stack da task HTTP.
    const size_t cap = 10240;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = buildPortalLog(irrigationModule->auditLogRef(), buf, cap);
    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
    free(buf);
}

static void hGpo(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[256];
    size_t nb = readBody(req, body, sizeof(body));
    PortalGpoReq g;
    ParseResult pr = parseGpoReq(body, nb, g);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalGpo(g)) {
        sendJson(res, "{\"errors\":[\"gpo rejeitado (modo seguro, id invalido ou biestavel sem confirmacao)\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

static void hCoordsGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    PortalCoords c = {};
    irrigationModule->portalGetCoords(c);
    char buf[128];
    if (!buildCoords(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hCoordsSet(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    PortalCoords c;
    ParseResult pr = parseCoords(body, nb, c);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalSetCoords(c)) {
        sendJson(res, "{\"errors\":[\"falha ao gravar coordenadas\"]}", 500);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

void registerIrrigationPortalHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/portal/node", "GET", &hNode));
    server->registerNode(new ResourceNode("/api/portal/node/pulse", "POST", &hNodePulse));
    server->registerNode(new ResourceNode("/api/portal/net/roster", "GET", &hNetRoster));
    server->registerNode(new ResourceNode("/api/portal/net/command", "POST", &hNetCommand));
    server->registerNode(new ResourceNode("/api/portal/sensors", "GET", &hSensors));
    server->registerNode(new ResourceNode("/api/portal/log", "GET", &hLog));
    server->registerNode(new ResourceNode("/api/portal/gpo", "POST", &hGpo));
    server->registerNode(new ResourceNode("/api/portal/coords", "GET", &hCoordsGet));
    server->registerNode(new ResourceNode("/api/portal/coords", "POST", &hCoordsSet));
}

#endif
