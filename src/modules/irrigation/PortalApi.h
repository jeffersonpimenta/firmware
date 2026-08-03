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

} // namespace IrrigationWeb
