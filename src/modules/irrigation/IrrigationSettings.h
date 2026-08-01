#pragma once
#include <cstddef>
#include <stdint.h>

enum class IrrigationRole : uint8_t { ESTACAO = 0, GATEWAY = 1, REPETIDOR = 2, SERVICO = 3 };

// Layout do blob on-disk/radio (180 bytes, ABI-locked v6):
//   0  magic(4) | 4  version(2) | 6  role(1) | 7  numValves(1)
//   8  boundGateway(4) | 12 hbMinutes(2) | 14 vbatMinAbrirCentiV(2) | 16 maxOpenConfigS(2)
//  18  cmdRatePerMin(1) | 19 pad0(1) | 20 pulseMs(2)
//  22  pinsHbridgeA[8] | 30 pinsHbridgeB[8] | 38 pad1[2]
//  40  configEpoch(4) | 44 pinsDigitalIn[4] | 48 digitalInActiveLow(1) | 49 pinBtn(1) | 50 pinLed(1) | 51 pad2(1)
// --- v4 (Fase 6a) ---
//  52  pinsGpo[2](2) | 54 pinTamper(1) | 55 hwFlags(1)
//  56  latE7(4) | 60 lonE7(4)
//  64  sensores[4]×16(64)
// --- v5 (Fase 6b) ---
// 128  localInterlocks[4]×12(48)
// --- v6 (Fase 9) ---
// 176  vbatAvisoCentiV(2) | 178 vbatCriticaCentiV(2)
// Total = 180.
struct IrrigationSettings {
    static constexpr uint32_t MAGIC = 0x49525231; // "IRR1"
    static constexpr uint8_t MAX_VALVES = 8;
    static constexpr uint8_t MAX_DIGITAL_IN = 4;

    uint32_t magic = MAGIC;
    uint16_t version = 6;
    uint8_t role = (uint8_t)IrrigationRole::ESTACAO;
    uint8_t numValves = 2;
    uint32_t boundGateway = 0; // 0 = não pareado
    uint16_t hbMinutes = 10;
    uint16_t vbatMinAbrirCentiV = 1180; // 11,8 V (spec §8.1)
    uint16_t maxOpenConfigS = 0;        // 0 = só o teto compilado limita
    uint8_t cmdRatePerMin = 10;
    uint8_t pad0 = 0;   // explicit padding at offset 19 (between cmdRatePerMin and pulseMs)
    uint16_t pulseMs = 60;
    int8_t pinsHbridgeA[MAX_VALVES] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int8_t pinsHbridgeB[MAX_VALVES] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint8_t pad1[2] = {0, 0}; // explicit padding at offsets 38-39 (after pinsHbridgeB)
    // v2:
    uint32_t configEpoch = 0;                        // spec §5.4
    int8_t pinsDigitalIn[MAX_DIGITAL_IN] = {-1, -1, -1, -1};
    uint8_t digitalInActiveLow = 0; // bitmask: bit i = entrada i ativo-baixo (polaridade configurável)
    // v3:
    int8_t pinBtn = -1;   // botão multifunção (§8.6); -1 = ausente
    int8_t pinLed = -1;   // LED de status (§8.7); -1 = ausente
    uint8_t pad2 = 0;    // explicit padding at offset 51 (end of v3 prefix)

    // v4 (Fase 6a):
    static constexpr uint8_t MAX_GPO = 2;
    static constexpr uint8_t MAX_SENSORS = 4;

    struct SensorSlot {
        int8_t pino = -1;        // -1 = slot vazio
        uint8_t tipo = 0;        // 0 = digital, 1 = analógico
        uint8_t flags = 0;       // bit0 = ativo-baixo (digital)
        uint8_t amostragemS = 0; // analógico; 0 → default 30
        uint16_t debounceMs = 0; // digital; 0 → default 200
        uint16_t adcMin = 0;     // calibração 2 pontos (analógico)
        uint16_t adcMax = 4095;
        int16_t engMin = 0;      // centi-unidades (1000 = 10,00)
        int16_t engMax = 0;
        uint8_t unidade = 0;     // 0 raw, 1 bar, 2 %, 3 m, 4 °C
        uint8_t pad = 0;
    };

    int8_t pinsGpo[MAX_GPO] = {-1, -1}; // saídas de nível (§8.11)
    int8_t pinTamper = -1;              // §8.12; -1 desativa
    uint8_t hwFlags = 0;                // bit0 = tamper ativo-baixo
    int32_t latE7 = 0;                  // coordenadas locais ×1e-7 (§8.8)
    int32_t lonE7 = 0;
    SensorSlot sensores[MAX_SENSORS];

    // v5 (Fase 6b): réplica local de intertravamentos (§8.10).
    static constexpr uint8_t MAX_LOCAL_INTERLOCKS = 4;
    struct LocalInterlock {
        uint8_t sensorIdx = 0;        // 0..3 (sensor local)
        uint8_t condicao = 0;         // InterlockCond (0=ATIVO,1=INATIVO,2=MENOR_QUE,3=MAIOR_QUE)
        uint8_t acao = 0;             // InterlockAcao (0=BLOQUEAR_ABERTURA,1=FECHAR_E_BLOQUEAR)
        uint8_t saidasValvMask = 0;   // bits = índices de válvula locais a fechar+bloquear
        int32_t valorCenti = 0;
        uint16_t histereseCenti = 0;
        uint8_t saidasGpoMask = 0;    // bits = índices de GPO locais a fechar+bloquear
        uint8_t pad = 0;
        // slot inativo quando saidasValvMask==0 && saidasGpoMask==0
    };
    LocalInterlock localInterlocks[MAX_LOCAL_INTERLOCKS];

    // v6 (Fase 9): limiares de bateria por-estação (centi-volt), avaliados gateway-side
    // (StationMonitor) e disponíveis ao nó para auto-avaliação futura. Apêndice no FIM
    // para preservar todos os offsets v5.
    uint16_t vbatAvisoCentiV = 1220;   // default = StationMonitor::AVISO_CV
    uint16_t vbatCriticaCentiV = 1180; // default = StationMonitor::CRITICO_CV
};

static constexpr size_t IRRIGATION_SETTINGS_V1_SIZE = 40;
static constexpr size_t IRRIGATION_SETTINGS_V3_SIZE = 52;
static constexpr size_t IRRIGATION_SETTINGS_V4_SIZE = 128;
static constexpr size_t IRRIGATION_SETTINGS_V5_SIZE = 176;
static_assert(sizeof(IrrigationSettings::SensorSlot) == 16, "SensorSlot é ABI on-disk");
static_assert(sizeof(IrrigationSettings::LocalInterlock) == 12, "LocalInterlock é ABI on-disk");
static_assert(offsetof(IrrigationSettings, localInterlocks) == 128, "ABI v5");

// ABI lock v6: prefixo v5 (176 B) + vbatAvisoCentiV(2) + vbatCriticaCentiV(2) = 180.
// Prefixo v5 = prefixo v4 (128 B) + localInterlocks[4×12](48) = 176.
// Prefixo v4: magic(4)+version(2)+role(1)+numValves(1)+boundGateway(4)+hbMinutes(2)+
// vbatMinAbrirCentiV(2)+maxOpenConfigS(2)+cmdRatePerMin(1)+pad0(1)+pulseMs(2)+
// pinsHbridgeA(8)+pinsHbridgeB(8)+pad1(2)+configEpoch(4)+pinsDigitalIn(4)+
// digitalInActiveLow(1)+pinBtn(1)+pinLed(1)+pad2(1)+pinsGpo(2)+pinTamper(1)+hwFlags(1)+
// latE7(4)+lonE7(4)+sensores[4×16](64) = 128. All padding explicit and zero-initialized.
// Bump version AND these asserts on any layout change.
static_assert(offsetof(IrrigationSettings, vbatAvisoCentiV) == 176, "ABI v6");
static_assert(offsetof(IrrigationSettings, vbatCriticaCentiV) == 178, "ABI v6");
static_assert(sizeof(IrrigationSettings) == 180, "on-disk settings format is ABI-dependent; bump version on layout change");

// Pino de offsets do apêndice v4: drift silencioso de layout vira erro de compilação.
static_assert(offsetof(IrrigationSettings, pinsGpo) == 52, "ABI v4");
static_assert(offsetof(IrrigationSettings, pinTamper) == 54, "ABI v4");
static_assert(offsetof(IrrigationSettings, hwFlags) == 55, "ABI v4");
static_assert(offsetof(IrrigationSettings, latE7) == 56, "ABI v4");
static_assert(offsetof(IrrigationSettings, lonE7) == 60, "ABI v4");
static_assert(offsetof(IrrigationSettings, sensores) == 64, "ABI v4");

// Blob v1, v2, v3, v4 ou v5 → struct v5. false = magic/versão/tamanho inválido (out fica intacto).
bool migrateIrrigationSettings(const uint8_t *raw, size_t n, IrrigationSettings &out);

// false = arquivo ausente/corrompido; `s` fica com os defaults acima.
bool loadIrrigationSettings(IrrigationSettings &s);
bool saveIrrigationSettings(const IrrigationSettings &s);
