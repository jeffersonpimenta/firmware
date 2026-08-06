#pragma once
#include "modules/irrigation/AuditLog.h"
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/IrrigationWebApi.h" // JsonWriter/JsonReader/ParseResult (reuso)
#include <cstddef>
#include <cstdint>

namespace IrrigationWeb
{

// --- Aba "Este nó" ---
struct NodeStateCtx {
    uint8_t role = 0;          // IrrigationRole
    const char *name = "";
    uint32_t boundGateway = 0; // 0 = não pareado
    uint32_t configEpoch = 0;
    bool safeMode = false;
    bool provisioned = false; // false = nó de fábrica (sem config salva) → wizard de 1º boot (§6)
    uint8_t numValves = 0;
    uint8_t numGpos = 0;
    uint8_t valveStates = 0;   // bitmap
    uint8_t gpoStates = 0;     // bitmap
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0; // 0 se não medido
    uint8_t flags = 0;         // HbFlags (tamper/safe/hibernation)
    uint32_t apSecondsLeft = 0;
    uint32_t uptimeS = 0; // segundos desde o boot (millis()/1000)
    uint32_t nowEpoch = 0; // getValidTime local (0 = sem relógio) — display-only (fase 8b)
    bool hasTime = false;
};
size_t buildNodeState(const NodeStateCtx &ctx, char *buf, size_t cap);

struct PortalPulseReq {
    uint8_t valveId = 0;
    uint16_t durationS = 0;
};
ParseResult parsePulse(const char *json, size_t len, PortalPulseReq &out);

// --- Provisionamento de 1º boot (wizard de papel, §6) ---
struct ProvisionReq {
    uint8_t role = 0;        // IrrigationRole 0..3
    char farmName[13] = {0}; // opcional; vira o nome do canal quando role==GATEWAY
    bool hasFarmName = false;
};
ParseResult parseProvision(const char *json, size_t len, ProvisionReq &out);

// --- Aba "Rede" ---
struct NetCommand {
    uint8_t zoneId = 0;
    uint8_t action = 0; // 0 = fechar, 1 = abrir
    uint16_t durationS = 0;
};
ParseResult parseNetCommand(const char *json, size_t len, NetCommand &out);

// Lista de alvos. zones != nullptr (gateway) => emite as zonas; nullptr (estação) => "[]".
size_t buildRoster(const ZoneTable *zones, char *buf, size_t cap);

// --- Sensores / GPO / coords / mini-log (Fase 6a) ---
struct PortalSensorItem {
    uint8_t id = 0;
    uint8_t tipo = 0;    // 0 digital, 1 analógico
    uint8_t unidade = 0; // enum settings v4: 0 raw, 1 bar, 2 %, 3 m, 4 °C
    int16_t valueCenti = 0;
};
struct PortalSensorsCtx {
    uint8_t count = 0;
    PortalSensorItem items[4];
};
size_t buildSensors(const PortalSensorsCtx &ctx, char *buf, size_t cap);

// Serializa até 100 registros. Pior caso ~96 B/registro → ~9,6 KB para um log cheio;
// o endpoint (Task 10) deve alocar ~10 KB (heap, não stack no ESP32) e checar done()==0.
size_t buildPortalLog(const AuditLog &log, char *buf, size_t cap);

struct PortalGpoReq {
    uint8_t gpoId = 0;
    uint8_t action = 0;   // 0 = desligar, 1 = ligar
    uint16_t durationS = 0;
    bool confirm = false; // biestável (durationS==0 ao ligar) exige confirm=true
};
ParseResult parseGpoReq(const char *json, size_t len, PortalGpoReq &out);

struct PortalCoords {
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
};
size_t buildCoords(const PortalCoords &c, char *buf, size_t cap);
ParseResult parseCoords(const char *json, size_t len, PortalCoords &out);

// --- Aba "Enlace" (repetidor, §7.2) ---
struct LinkNeighbor {
    uint32_t node = 0;
    int8_t snrQuarterDb = 0;
    uint8_t hops = 0;
    char name[16] = {0};
};
struct LinkCtx {
    int8_t snrQuarterDb = 0;  // enlace ao gateway (último rx)
    int16_t rssiDbm = 0;
    uint8_t histCount = 0;
    uint8_t hist[12] = {0};   // amostras já normalizadas 0..100 p/ a barra
    uint8_t neighborCount = 0;
    LinkNeighbor neighbors[8];
};
size_t buildLink(const LinkCtx &ctx, char *buf, size_t cap);

// --- Aba "Rede Wi-Fi" (fase 8a) ---
struct WifiStatusCtx {
    bool enabled = false;         // config.network.wifi_enabled
    bool staUp = false;           // WiFi.isConnected()
    char connectedSsid[33] = {0}; // "" se não conectado
    char ip[16] = {0};            // "" se sem IP
};
size_t buildWifiStatus(const WifiStatusCtx &ctx, char *buf, size_t cap);

struct WifiScanItem {
    char ssid[33] = {0};
    int16_t rssi = 0;
    bool secure = true;
};
// ~600 B: alocar no heap do endpoint (não no stack da task HTTP), como buildPortalLog.
struct WifiScanCtx {
    bool scanning = false; // true → frontend mostra spinner, ignora items
    uint8_t count = 0;     // <= 16
    WifiScanItem items[16];
};
size_t buildWifiScan(const WifiScanCtx &ctx, char *buf, size_t cap);

struct WifiConnectReq {
    char ssid[33] = {0};
    char psk[64] = {0}; // vazio = rede aberta
};
ParseResult parseWifiConnect(const char *json, size_t len, WifiConnectReq &out);

enum class WifiConnectState : uint8_t { Idle = 0, Connecting = 1, Success = 2, Error = 3 };
struct WifiConnectCtx {
    WifiConnectState state = WifiConnectState::Idle;
    char ssid[33] = {0};
    char error[48] = {0}; // sempre emitido; string vazia salvo quando state==Error
};
size_t buildWifiConnect(const WifiConnectCtx &ctx, char *buf, size_t cap);

struct WifiToggleReq { bool enabled = false; };
ParseResult parseWifiToggle(const char *json, size_t len, WifiToggleReq &out);

} // namespace IrrigationWeb
