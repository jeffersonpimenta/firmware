#include "modules/irrigation/IrrigationPortalEndpoints.h"
#if !MESHTASTIC_EXCLUDE_WEBSERVER

#include "modules/irrigation/IrrigationEndpointHelpers.h" // sendJson/sendParseErrors/readBody
#include "modules/irrigation/IrrigationModule.h"
#include "modules/irrigation/IrrigationWebApi.h"    // JsonReader
#include "modules/irrigation/PortalApi.h"
#include "modules/irrigation/ServicePortalApi.h" // buildStationConfig (hNodeBackup)

#include <Arduino.h>
#include <cstdlib> // malloc/free explícitos (hLog aloca ~10 KB no heap)

#undef str
#include <HTTPServer.hpp> // tipo completo p/ server->registerNode() (ResourceNode.hpp só faz fwd-decl)
#include <ResourceNode.hpp>

using namespace httpsserver;
using namespace IrrigationWeb;

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
    // buildPortalLog devolve n bytes sem null-terminar; sendJson usa strlen e vazaria
    // lixo do heap após o JSON. Termina explicitamente antes de enviar.
    if (n >= cap)
        n = cap - 1;
    buf[n] = '\0';
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

static void hLink(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    LinkCtx c = {};
    irrigationModule->portalFillLink(c);
    char buf[1024]; // até 8 vizinhos + histórico não cabem em 512
    if (!buildLink(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// ── Site survey (§8.5) — modo beacon de cobertura (nó §7.2) ──────────────────

static void hSurveyStart(HTTPRequest *req, HTTPResponse *res)
{
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

static void hSurveyStop(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    irrigationModule->portalStopSurvey();
    sendJson(res, "{\"ok\":true}");
}

static void hProvision(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[192];
    size_t nb = readBody(req, body, sizeof(body));
    ProvisionReq p;
    ParseResult pr = parseProvision(body, nb, p);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalProvision(p)) {
        sendJson(res, "{\"errors\":[\"provisionamento rejeitado\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true,\"reboot\":true}");
}

static void hOtaStatus(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    OtaStatusCtx c = {};
    irrigationModule->portalFillOtaStatus(c);
    char buf[256];
    if (!buildOtaStatus(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hOtaArm(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    OtaArmReq r;
    ParseResult pr = parseOtaArm(body, nb, r);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalOtaArm(r)) {
        sendJson(res, "{\"errors\":[\"OTA recusado (ciclo ativo ou loader ausente/sem BLE)\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true,\"reboot\":true}");
}

// GET /api/node/node/backup — baixa a config deste nó (sem PSK), p/ restaurar depois.
static void hNodeBackup(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    const size_t cap = 4096;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        res->setStatusCode(500);
        return;
    }
    size_t n = buildStationConfig(irrigationModule->getSettings(), buf, cap);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Content-Disposition", "attachment; filename=\"no-backup.json\"");
    res->write((uint8_t *)buf, n);
    free(buf);
}

// POST /api/node/node/restore — reaplica um backup neste nó; mantém vínculo, sobe epoch, avisa gw.
static void hNodeRestore(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) { res->setStatusCode(404); return; }
    char body[4096];
    size_t nb = readBody(req, body, sizeof(body));
    uint32_t epoch = 0;
    if (!irrigationModule->portalRestoreConfig(body, nb, epoch)) {
        sendJson(res, "{\"ok\":false,\"errors\":[\"backup invalido\"]}", 400);
        return;
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"epoch\":%u}", (unsigned)epoch);
    sendJson(res, out);
}

static void hNodeReboot(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    sendJson(res, "{\"ok\":true,\"reboot\":true}"); // responde antes do reboot
    irrigationModule->portalReboot();
}

static void hNodeReset(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) { res->setStatusCode(404); return; }
    sendJson(res, "{\"ok\":true,\"reboot\":true}"); // responde antes do reboot
    irrigationModule->portalFactoryReset();
}

// ── Wi-Fi management (§7) ────────────────────────────────────────────────────
// WiFi só existe no gateway (nós são bateria/solar — não provisionam WiFi nem varrem redes).
// Todos os endpoints WiFi respondem 404 fora do gateway.
static bool wifiOnGateway()
{
    return irrigationModule && irrigationModule->gwIsGateway();
}

static void hWifiStatus(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!wifiOnGateway()) {
        res->setStatusCode(404);
        return;
    }
    WifiStatusCtx c = {};
    irrigationModule->portalWifiStatus(c);
    char buf[160];
    if (!buildWifiStatus(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hWifiScanStart(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!wifiOnGateway()) {
        res->setStatusCode(404);
        return;
    }
    irrigationModule->portalWifiStartScan();
    sendJson(res, "{\"ok\":true}");
}

static void hWifiScanResult(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!wifiOnGateway()) {
        res->setStatusCode(404);
        return;
    }
    WifiScanCtx c = {};
    irrigationModule->portalWifiScanResult(c);
    char buf[1024]; // 16 redes * ~56 B
    if (!buildWifiScan(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

// Escrita de rede (connect/forget/toggle) exige a janela do portal aberta (botão do gateway
// pressionado). Com STA sempre-on os endpoints ficam alcançáveis pela LAN; sem esta trava
// qualquer um na LAN reconfiguraria o Wi-Fi. Leitura (status/scan) permanece livre.
static bool wifiWriteAllowed()
{
    return wifiOnGateway() && irrigationModule->portalSession().apShouldBeUp();
}

static void hWifiConnectStart(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    if (!wifiWriteAllowed()) {
        sendJson(res, "{\"errors\":[\"portal fechado — pressione o botao do gateway\"]}", 403);
        return;
    }
    char body[160];
    size_t nb = readBody(req, body, sizeof(body));
    WifiConnectReq p;
    ParseResult pr = parseWifiConnect(body, nb, p);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    if (!irrigationModule->portalWifiConnect(p)) {
        sendJson(res, "{\"errors\":[\"conexao rejeitada\"]}", 400);
        return;
    }
    sendJson(res, "{\"ok\":true}");
}

static void hWifiConnectProgress(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!wifiOnGateway()) {
        res->setStatusCode(404);
        return;
    }
    WifiConnectCtx c = {};
    irrigationModule->portalWifiConnectProgress(c);
    char buf[160];
    if (!buildWifiConnect(c, buf, sizeof(buf))) {
        res->setStatusCode(500);
        return;
    }
    sendJson(res, buf);
}

static void hWifiForget(HTTPRequest *req, HTTPResponse *res)
{
    (void)req;
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    if (!wifiWriteAllowed()) {
        sendJson(res, "{\"errors\":[\"portal fechado — pressione o botao do gateway\"]}", 403);
        return;
    }
    irrigationModule->portalWifiForget();
    sendJson(res, "{\"ok\":true}");
}

static void hWifiToggle(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) {
        res->setStatusCode(404);
        return;
    }
    if (!wifiWriteAllowed()) {
        sendJson(res, "{\"errors\":[\"portal fechado — pressione o botao do gateway\"]}", 403);
        return;
    }
    char body[64];
    size_t nb = readBody(req, body, sizeof(body));
    WifiToggleReq t;
    ParseResult pr = parseWifiToggle(body, nb, t);
    if (!pr.ok) {
        sendParseErrors(res, pr);
        return;
    }
    irrigationModule->portalWifiToggle(t.enabled);
    sendJson(res, "{\"ok\":true}");
}

// POST /api/node/ble — janela BLE sob demanda (5/10/15 min): WiFi do portal cai, BLE on por N min,
// device reboota e volta sozinho. Espelha hBleWindow do gateway.
static void hPortalBle(HTTPRequest *req, HTTPResponse *res)
{
    if (!irrigationModule) { res->setStatusCode(404); return; }
    char body[128];
    size_t nb = readBody(req, body, sizeof(body));
    JsonReader rd(body, nb);
    int64_t minutes = 0;
    if (!rd.getInt("minutes", minutes) || (minutes != 5 && minutes != 10 && minutes != 15)) {
        sendJson(res, "{\"ok\":false,\"errors\":[\"minutes deve ser 5, 10 ou 15\"]}", 400);
        return;
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"minutes\":%d}", (int)minutes);
    sendJson(res, out); // responde antes do reboot
    irrigationModule->startBleWindow((uint8_t)minutes);
}

void registerIrrigationPortalHandlers(HTTPServer *server)
{
    server->registerNode(new ResourceNode("/api/node/provision", "POST", &hProvision));
    server->registerNode(new ResourceNode("/api/node/node", "GET", &hNode));
    server->registerNode(new ResourceNode("/api/node/node/backup", "GET", &hNodeBackup));
    server->registerNode(new ResourceNode("/api/node/node/restore", "POST", &hNodeRestore));
    server->registerNode(new ResourceNode("/api/node/node/reboot", "POST", &hNodeReboot));
    server->registerNode(new ResourceNode("/api/node/node/reset", "POST", &hNodeReset));
    server->registerNode(new ResourceNode("/api/node/ble", "POST", &hPortalBle));
    server->registerNode(new ResourceNode("/api/node/node/pulse", "POST", &hNodePulse));
    server->registerNode(new ResourceNode("/api/node/net/roster", "GET", &hNetRoster));
    server->registerNode(new ResourceNode("/api/node/net/command", "POST", &hNetCommand));
    server->registerNode(new ResourceNode("/api/node/sensors", "GET", &hSensors));
    server->registerNode(new ResourceNode("/api/node/log", "GET", &hLog));
    server->registerNode(new ResourceNode("/api/node/gpo", "POST", &hGpo));
    server->registerNode(new ResourceNode("/api/node/coords", "GET", &hCoordsGet));
    server->registerNode(new ResourceNode("/api/node/coords", "POST", &hCoordsSet));
    server->registerNode(new ResourceNode("/api/node/survey/start", "POST", &hSurveyStart));
    server->registerNode(new ResourceNode("/api/node/survey/stop", "POST", &hSurveyStop));
    server->registerNode(new ResourceNode("/api/node/link", "GET", &hLink));
    server->registerNode(new ResourceNode("/api/node/ota/status", "GET", &hOtaStatus));
    server->registerNode(new ResourceNode("/api/node/ota", "POST", &hOtaArm));
    server->registerNode(new ResourceNode("/api/node/wifi", "GET", &hWifiStatus));
    server->registerNode(new ResourceNode("/api/node/wifi/scan", "POST", &hWifiScanStart));
    server->registerNode(new ResourceNode("/api/node/wifi/scan", "GET", &hWifiScanResult));
    server->registerNode(new ResourceNode("/api/node/wifi/connect", "POST", &hWifiConnectStart));
    server->registerNode(new ResourceNode("/api/node/wifi/connect", "GET", &hWifiConnectProgress));
    server->registerNode(new ResourceNode("/api/node/wifi/forget", "POST", &hWifiForget));
    server->registerNode(new ResourceNode("/api/node/wifi/toggle", "POST", &hWifiToggle));
}

#endif
