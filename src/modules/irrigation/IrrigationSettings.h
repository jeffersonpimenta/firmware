#pragma once
#include <cstddef>
#include <stdint.h>

enum class IrrigationRole : uint8_t { ESTACAO = 0, GATEWAY = 1, REPETIDOR = 2, SERVICO = 3 };

// Camada 1 mínima (pin map + parâmetros) da spec §5.1/§5.3. Formato completo
// (sensores, staging atômico em NVS, epoch) chega na Fase 2.
struct IrrigationSettings {
    static constexpr uint32_t MAGIC = 0x49525231; // "IRR1"
    static constexpr uint8_t MAX_VALVES = 8;
    static constexpr uint8_t MAX_DIGITAL_IN = 4;

    uint32_t magic = MAGIC;
    uint16_t version = 2;
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
    // v2:
    uint32_t configEpoch = 0;                        // spec §5.4
    int8_t pinsDigitalIn[MAX_DIGITAL_IN] = {-1, -1, -1, -1};
    uint8_t digitalInActiveLow = 0; // bitmask: bit i = entrada i ativo-baixo (polaridade configurável)
};

static constexpr size_t IRRIGATION_SETTINGS_V1_SIZE = 40;

// ABI lock v2: v1(40) + configEpoch(4) + pinsDigitalIn(4) + digitalInActiveLow(1) + pad(3) = 52.
// Bump version AND this assert on any layout change.
static_assert(sizeof(IrrigationSettings) == 52, "on-disk settings format is ABI-dependent; bump version on layout change");

// Blob v1 ou v2 → struct v2. false = magic/versão/tamanho inválido (out fica intacto).
bool migrateIrrigationSettings(const uint8_t *raw, size_t n, IrrigationSettings &out);

// false = arquivo ausente/corrompido; `s` fica com os defaults acima.
bool loadIrrigationSettings(IrrigationSettings &s);
bool saveIrrigationSettings(const IrrigationSettings &s);
