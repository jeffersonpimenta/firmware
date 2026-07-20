#pragma once
#include "modules/irrigation/IrrigationProtocol.h" // SensorReading
#include "modules/irrigation/IrrigationSettings.h"
#include <stddef.h>
#include <stdint.h>

// Leitor injetado: implementação Arduino fica no módulo; testes usam mock.
class ISensorReader
{
  public:
    virtual ~ISensorReader() = default;
    virtual uint16_t readAdc(int8_t pin) = 0;
    virtual bool readLevel(int8_t pin) = 0; // nível bruto, sem polaridade
};

// Processa leituras de sensores digitais e analógicos com debounce,
// calibração 2-pontos, histerese e sinalização de heartbeat antecipado.
class SensorSampler
{
  public:
    static constexpr uint16_t DEFAULT_DEBOUNCE_MS = 200;
    static constexpr uint8_t DEFAULT_AMOSTRAGEM_S = 30;
    static constexpr uint32_t EARLY_HB_MIN_INTERVAL_MS = 30u * 1000u;
    static constexpr uint8_t HYST_PCT = 2; // % do span de engenharia

    void configure(const IrrigationSettings &s);
    void tick(uint32_t nowMs, ISensorReader &rd);
    // Preenche leituras válidas; devolve quantas (0..MAX_SENSORS).
    size_t readings(IrrigationProto::SensorReading out[IrrigationSettings::MAX_SENSORS]) const;
    // Condição viva, não latch — transiente que volta para a banda não dispara.
    bool earlyHeartbeatDue(uint32_t nowMs) const;
    void noteReported(uint32_t nowMs); // chamar após TODO envio de heartbeat

  private:
    struct SlotState {
        bool valid = false;
        int16_t valueCenti = 0;
        bool lastRaw = false;
        uint32_t rawSinceMs = 0;
        bool everSampled = false;
        uint32_t lastSampleMs = 0;
        bool everReported = false;
        int16_t reportedCenti = 0;
    };
    IrrigationSettings cfg;
    SlotState st[IrrigationSettings::MAX_SENSORS];
    uint32_t lastReportMs = 0;

    int16_t calibrate(const IrrigationSettings::SensorSlot &sl, uint16_t adc) const;
    // Calcula banda de histerese analógica para um slot (mínimo 1 centi).
    int32_t analogBand(const IrrigationSettings::SensorSlot &sl) const;
};
