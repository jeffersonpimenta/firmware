#pragma once
#include <cstddef>
#include <cstdint>

struct StationTelemetry {
    uint32_t node = 0; // 0 = slot vazio
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0;
    int8_t snrQuarterDb = 0;
    uint16_t rebootCount = 0;
    uint8_t flags = 0;
    uint32_t configEpoch = 0;
    uint32_t atMs = 0;
};

// Último heartbeat por estação (só RAM; não persiste — reconstrói ao ouvir de novo).
class StationTelemetryCache
{
  public:
    static constexpr size_t MAX = 16;
    void update(const StationTelemetry &t); // upsert por node
    const StationTelemetry *byNode(uint32_t node) const;

  private:
    StationTelemetry entries[MAX] = {};
};
