#include "modules/irrigation/ServicePortalEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "FSCommon.h"
#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/ServicePortalApi.h"

#include <Arduino.h>
#include <cstdio>  // snprintf (mensagem de erro do import)
#include <cstdlib> // malloc/free (buffers grandes vão no heap, não na pilha da task HTTP)
#include <cstring>

#undef str
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <ResourceNode.hpp>

using namespace httpsserver;
using namespace IrrigationWeb;

// Helpers espelham IrrigationPortalEndpoints.cpp (Fase 5b).
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

// Todo handler responde 404 fora do role SERVICO (§11.8).
static bool svcGuard(HTTPResponse *res)
{
    if (!irrigationModule || !irrigationModule->svcIsService()) {
        res->setStatusCode(404);
        return false;
    }
    return true;
}

// ── Aba Clientes ─────────────────────────────────────────────────────────────

static void hSvcClients(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res))
        return;
    char *buf = (char *)malloc(2560);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = irrigationModule->svcPortalListClients(buf, 2560);
    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
    free(buf);
}

static void hSvcSelect(HTTPRequest *req, HTTPResponse *res)
{
    if (!svcGuard(res))
        return;
    char body[64];
    size_t nb = readBody(req, body, sizeof body);
    char id[32] = {0};
    ParseResult pr = parseSelect(body, nb, id, sizeof id);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->svcPortalSelect(id)) {
        sendJson(res, "{\"errors\":[\"cliente inexistente ou PSK invalida\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true,\"rebooting\":true}", 202); // re-tune + reboot em 3 s
}

// ── Aba Rede — varredura ─────────────────────────────────────────────────────

static void hSvcScanStart(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res))
        return;
    irrigationModule->svcPortalStartScan();
    sendJson(res, "{\"ok\":true}");
}

static void hSvcScanGet(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res))
        return;
    char *buf = (char *)malloc(4096);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = irrigationModule->svcPortalScanResults(buf, 4096);
    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
    free(buf);
}

// ── Aba Rede — leitura/escrita de config ─────────────────────────────────────

static void hSvcConfigRead(HTTPRequest *req, HTTPResponse *res) // POST {node}
{
    if (!svcGuard(res))
        return;
    char body[64];
    size_t nb = readBody(req, body, sizeof body);
    char nodeStr[16] = {0};
    JsonReader rd(body, nb);
    if (!rd.getStr("node", nodeStr, sizeof nodeStr)) {
        sendJson(res, "{\"errors\":[\"node ausente\"]}", 400);
        return;
    }
    uint32_t node = IrrigationService::parseNodeHex(nodeStr);
    if (!node) {
        sendJson(res, "{\"errors\":[\"node invalido\"]}", 400);
        return;
    }
    // 1ª chamada dispara GET_CONFIG; o cliente faz poll até o reply chegar (§11.6).
    if (!irrigationModule->svcPortalConfigReady(node)) {
        irrigationModule->svcPortalReadConfig(node);
        sendJson(res, "{\"pending\":true}", 202);
        return;
    }
    char *buf = (char *)malloc(3072);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = irrigationModule->svcPortalGetReadConfig(buf, 3072);
    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
    free(buf);
}

static void hSvcConfigWrite(HTTPRequest *req, HTTPResponse *res) // POST {node,route,config}
{
    if (!svcGuard(res))
        return;
    char *body = (char *)malloc(3072);
    if (!body) {
        res->setStatusCode(500);
        return;
    }
    size_t nb = readBody(req, body, 3072);
    // node p/ semear os campos geridos do último blob lido (magic/role/boundGateway/epoch),
    // que o editor NÃO edita — sem semente, a escrita zeraria boundGateway/epoch.
    char nodeStr[16] = {0};
    {
        JsonReader rd(body, nb);
        rd.getStr("node", nodeStr, sizeof nodeStr);
    }
    uint32_t node = IrrigationService::parseNodeHex(nodeStr);
    NodeConfigReq cr;
    if (!irrigationModule->svcPortalSeedConfig(node, cr.config)) {
        free(body);
        sendJson(res, "{\"errors\":[\"leia a config do no primeiro\"]}", 409);
        return;
    }
    ParseResult pr = parseNodeConfigReq(body, nb, cr);
    if (!pr.ok) {
        free(body);
        sendParseErrors(res, pr);
        return;
    }
    bool ok = irrigationModule->svcPortalWriteConfig(cr);
    free(body);
    sendJson(res,
             ok ? "{\"ok\":true}" : "{\"errors\":[\"escrita rejeitada (rota via-gateway nao suportada; use direta)\"]}",
             ok ? 200 : 400);
}

static void hSvcAction(HTTPRequest *req, HTTPResponse *res)
{
    if (!svcGuard(res))
        return;
    char body[192];
    size_t nb = readBody(req, body, sizeof body);
    NodeAction a;
    ParseResult pr = parseNodeAction(body, nb, a);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    bool ok = irrigationModule->svcPortalNodeAction(a);
    sendJson(res, ok ? "{\"ok\":true}" : "{\"errors\":[\"acao rejeitada\"]}", ok ? 200 : 400);
}

// ── Aba Log ──────────────────────────────────────────────────────────────────

static void hSvcLog(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res))
        return;
    const size_t cap = 10240;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = irrigationModule->svcPortalBuildLog(buf, cap);
    if (!n) {
        free(buf);
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
    free(buf);
}

// ── Import / export (§11.7) ──────────────────────────────────────────────────

static void hSvcExport(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res))
        return;
    const size_t cap = 32768; // teto do envelope multi-cliente (plaintext); overflow → 500
    char *buf = (char *)malloc(cap);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = irrigationModule->svcPortalExport(buf, cap);
    if (!n) {
        free(buf);
        sendJson(res, "{\"errors\":[\"export overflow (>32KB)\"]}", 500);
        return;
    }
    res->setStatusCode(200);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Content-Disposition", "attachment; filename=\"irrig-vault.json\"");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->print(buf); // JsonWriter mantém NUL-terminação em [n]
    free(buf);
}

static void doImport(HTTPRequest *req, HTTPResponse *res, bool replace)
{
    if (!svcGuard(res))
        return;
#ifdef FSCom
    // Stream do corpo POST → arquivo de staging (não segura o envelope inteiro na pilha); valida antes de aplicar.
    const char *staging = "/clientes/import.tmp";
    auto f = FSCom.open(staging, FILE_O_WRITE);
    if (!f) {
        res->setStatusCode(500);
        return;
    }
    uint8_t chunk[512];
    size_t total = 0;
    for (;;) {
        size_t r = req->readBytes(reinterpret_cast<byte *>(chunk), sizeof chunk);
        if (r == 0)
            break;
        f.write(chunk, r);
        total += r;
        if (total > 65536) {
            f.close();
            FSCom.remove(staging);
            sendJson(res, "{\"errors\":[\"import >64KB\"]}", 413);
            return;
        }
    }
    f.close();
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        FSCom.remove(staging);
        res->setStatusCode(500);
        return;
    }
    auto rf = FSCom.open(staging, FILE_O_READ);
    size_t got = rf ? rf.read((uint8_t *)buf, total) : 0;
    if (rf)
        rf.close();
    buf[got] = 0;
    FSCom.remove(staging);
    char err[64] = {0};
    bool ok = irrigationModule->svcPortalImport(buf, got, replace, err, sizeof err);
    free(buf);
    if (!ok) {
        char m[128];
        snprintf(m, sizeof m, "{\"errors\":[\"%s\"]}", err[0] ? err : "import invalido");
        sendJson(res, m, 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
#else
    (void)req;
    (void)replace;
    res->setStatusCode(500);
#endif
}

static void hSvcImport(HTTPRequest *req, HTTPResponse *res) { doImport(req, res, false); }
static void hSvcImportReplace(HTTPRequest *req, HTTPResponse *res) { doImport(req, res, true); }

// ── Site survey (§8.5) — device SERVICO em modo beacon ───────────────────────

static void hSvcSurveyStart(HTTPRequest *req, HTTPResponse *res)
{
    if (!svcGuard(res))
        return;
    char body[192];
    size_t nb = readBody(req, body, sizeof body);
    SurveyStartReq r;
    ParseResult pr = parseSurveyStart(body, nb, r);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    irrigationModule->portalStartSurvey(r);
    sendJson(res, "{\"ok\":true}");
}

static void hSvcSurveyStop(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!svcGuard(res))
        return;
    irrigationModule->portalStopSurvey();
    sendJson(res, "{\"ok\":true}");
}

void registerIrrigationServicePortalHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/portal/service/clients", "GET", &hSvcClients));
    server->registerNode(new ResourceNode("/api/portal/service/select", "POST", &hSvcSelect));
    server->registerNode(new ResourceNode("/api/portal/service/scan", "POST", &hSvcScanStart));
    server->registerNode(new ResourceNode("/api/portal/service/scan", "GET", &hSvcScanGet));
    server->registerNode(new ResourceNode("/api/portal/service/node/config/read", "POST", &hSvcConfigRead));
    server->registerNode(new ResourceNode("/api/portal/service/node/config/write", "POST", &hSvcConfigWrite));
    server->registerNode(new ResourceNode("/api/portal/service/node/action", "POST", &hSvcAction));
    server->registerNode(new ResourceNode("/api/portal/service/log", "GET", &hSvcLog));
    server->registerNode(new ResourceNode("/api/portal/service/export", "GET", &hSvcExport));
    server->registerNode(new ResourceNode("/api/portal/service/import", "POST", &hSvcImport));
    server->registerNode(new ResourceNode("/api/portal/service/import/replace", "POST", &hSvcImportReplace));
    server->registerNode(new ResourceNode("/api/portal/service/survey/start", "POST", &hSvcSurveyStart));
    server->registerNode(new ResourceNode("/api/portal/service/survey/stop", "POST", &hSvcSurveyStop));
}

#endif
