#pragma once

#include <cstdint>
#include <cstddef>

enum class AlertType : uint8_t {
    NONE = 0, BATT_AVISO, BATT_CRITICO, BATT_HIBERNACAO, BATT_RECUPEROU,
    SILENT, BACK_ONLINE, REBOOT_ANOMALY, CMD_FAIL, CONFIG_ADOPTED
};

struct Alert {
    AlertType type = AlertType::NONE;
    uint32_t node = 0;
    uint32_t arg = 0;
    uint32_t atMs = 0;
};

class AlertCenter {
  public:
    static constexpr size_t MAX = 32;

    void push(const Alert &a);
    size_t count() const;
    const Alert &at(size_t i) const;

  private:
    Alert ring[MAX] = {};
    size_t writeIdx = 0;
    size_t numAlerts = 0;
};

class StationMonitor {
  public:
    static constexpr size_t MAX = 16;
    static constexpr uint16_t HYST_CV = 20;       // 0,2 V
    static constexpr uint16_t AVISO_CV = 1220;
    static constexpr uint16_t CRITICO_CV = 1180;
    static constexpr uint16_t HIBER_CV = 1150;
    static constexpr uint16_t REBOOT_LIMIT_24H = 5;

    // Returns number of alerts written to out (0-3)
    // até 3 alertas: volta de silêncio + nível de bateria + reboot
    int onHeartbeat(uint32_t node, uint16_t vbatCentiV, uint16_t rebootCount, uint32_t nowMs, Alert out[3]);

    // Checks for silence; returns true if SILENT alert was generated
    bool checkSilence(uint32_t node, uint32_t silencioMs, uint32_t nowMs, Alert &out);

    // Returns time of last heartbeat for node, 0 if never heard
    uint32_t lastHeardMs(uint32_t node) const;

  private:
    struct StationState {
        uint32_t node = 0;
        uint32_t lastHeardMs = 0;
        uint8_t level = 0;               // 0=normal, 1=aviso, 2=critico, 3=hibernacao
        bool silentFired = false;
        uint16_t rebootBase = 0;
        uint32_t rebootWindowStartMs = 0;
        bool rebootInitialized = false;
        bool anomalyFired = false;
        bool everHeard = false;
    };

    StationState stations[MAX] = {};

    StationState *findOrCreate(uint32_t node, uint32_t nowMs);
    const StationState *find(uint32_t node) const;
    StationState *findMutable(uint32_t node);
    uint8_t calculateLevel(uint16_t vbatCentiV) const;
};
