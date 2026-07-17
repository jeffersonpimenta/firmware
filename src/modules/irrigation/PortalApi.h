#pragma once
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
    uint8_t numValves = 0;
    uint8_t valveStates = 0;   // bitmap
    uint8_t gpoStates = 0;     // bitmap
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0; // 0 se não medido
    uint8_t flags = 0;         // HbFlags (tamper/safe/hibernation)
    uint32_t apSecondsLeft = 0;
};
size_t buildNodeState(const NodeStateCtx &ctx, char *buf, size_t cap);

struct PortalPulseReq {
    uint8_t valveId = 0;
    uint16_t durationS = 0;
};
ParseResult parsePulse(const char *json, size_t len, PortalPulseReq &out);

// --- Aba "Rede" ---
struct NetCommand {
    uint8_t zoneId = 0;
    uint8_t action = 0; // 0 = fechar, 1 = abrir
    uint16_t durationS = 0;
};
ParseResult parseNetCommand(const char *json, size_t len, NetCommand &out);

// Lista de alvos. zones != nullptr (gateway) => emite as zonas; nullptr (estação) => "[]".
size_t buildRoster(const ZoneTable *zones, char *buf, size_t cap);

} // namespace IrrigationWeb
