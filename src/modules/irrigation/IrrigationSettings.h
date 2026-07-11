#pragma once
#include <stdint.h>

enum class IrrigationRole : uint8_t { ESTACAO = 0, GATEWAY = 1, REPETIDOR = 2, SERVICO = 3 };

// Camada 1 mínima (pin map + parâmetros) da spec §5.1/§5.3. Formato completo
// (sensores, staging atômico em NVS, epoch) chega na Fase 2.
struct IrrigationSettings {
    static constexpr uint32_t MAGIC = 0x49525231; // "IRR1"
    static constexpr uint8_t MAX_VALVES = 8;

    uint32_t magic = MAGIC;
    uint16_t version = 1;
    uint8_t role = (uint8_t)IrrigationRole::ESTACAO;
    uint8_t numValves = 2;
    uint32_t boundGateway = 0; // 0 = não pareado
    uint16_t hbMinutes = 10;
    uint16_t vbatMinAbrirCentiV = 1180; // 11,8 V (spec §8.1)
    uint16_t maxOpenConfigS = 0;        // 0 = só o teto compilado limita
    uint8_t cmdRatePerMin = 10;
    uint16_t pulseMs = 60;
    int8_t pinsHbridgeA[MAX_VALVES] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int8_t pinsHbridgeB[MAX_VALVES] = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// ABI lock: bump version field AND update this assert on any layout change.
// Layout: magic(4)+version(2)+role(1)+numValves(1)+boundGateway(4)+hbMinutes(2)+
//         vbatMinAbrirCentiV(2)+maxOpenConfigS(2)+cmdRatePerMin(1)+[pad1]+pulseMs(2)+
//         pinsHbridgeA[8]+pinsHbridgeB[8]+[pad2] = 40 bytes
static_assert(sizeof(IrrigationSettings) == 40, "on-disk settings format is ABI-dependent; bump version on layout change");

// false = arquivo ausente/corrompido; `s` fica com os defaults acima.
bool loadIrrigationSettings(IrrigationSettings &s);
bool saveIrrigationSettings(const IrrigationSettings &s);
