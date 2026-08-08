#pragma once
#include "modules/irrigation/IrrigationProtocol.h" // SensorReading, HB_MAX_SENSORS
#include <cstddef>
#include <cstdint>

struct StationTelemetry {
    uint32_t node = 0; // 0 = slot vazio
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0;
    int8_t snrQuarterDb = 0;
    int16_t rssiDbm = 0;
    uint16_t rebootCount = 0;
    uint8_t flags = 0;
    uint32_t configEpoch = 0;
    uint32_t atMs = 0;
    // Fase 6b: bloco de sensores do heartbeat (alimenta o InterlockEngine).
    uint8_t sensorCount = 0;
    IrrigationProto::SensorReading sensors[IrrigationProto::HB_MAX_SENSORS] = {};
    bool tamper = false; // HB_FLAG_TAMPER extraído
    // Modo Remoto (Task 6): estado real das saídas reportado pelo nó (HB + ACK).
    // Alimenta gwZoneIsOpen para zonas remotas.
    uint8_t valveStates = 0; // bit i = válvula i aberta
    uint8_t gpoStates = 0;   // bit i = GPO i ligado
};

// Último heartbeat por estação (só RAM; não persiste — reconstrói ao ouvir de novo).
class StationTelemetryCache
{
  public:
    static constexpr size_t MAX = 16;
    void update(const StationTelemetry &t); // upsert por node
    const StationTelemetry *byNode(uint32_t node) const;
    const StationTelemetry *entryAt(size_t i) const { return i < MAX ? &entries[i] : nullptr; }

  private:
    StationTelemetry entries[MAX] = {};
};
