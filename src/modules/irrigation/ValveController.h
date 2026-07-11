#pragma once
#include <stdint.h>

// Abstrai a ponte H (pulso latching). Implementação GPIO fica no módulo;
// testes usam mock.
class IValveDriver
{
  public:
    virtual ~IValveDriver() = default;
    virtual void pulse(uint8_t index, bool open) = 0;
};

// Timer fail-safe local (spec §4.2): a válvula fecha sozinha ao expirar,
// independente do rádio.
class ValveController
{
  public:
    static constexpr uint8_t MAX_VALVES = 8;
    static constexpr uint32_t MAX_OPEN_SECONDS = 120 * 60; // teto compilado

    enum class Result : uint8_t { OK, INVALID_ID, BATTERY_LOW, ZERO_DURATION };

    ValveController(IValveDriver &driver, uint8_t numValves)
        : driver(driver), numValves(numValves > MAX_VALVES ? MAX_VALVES : numValves)
    {
    }

    // Efetivo = min(durationS, configMaxS quando != 0, MAX_OPEN_SECONDS).
    // Já aberta: renova o timer sem novo pulso.
    Result open(uint8_t id, uint32_t durationS, uint32_t configMaxS, uint32_t nowMs);
    Result close(uint8_t id); // sempre aceito, mesmo em lockout de bateria
    void closeAll();
    void tick(uint32_t nowMs); // fecha válvulas com timer expirado
    bool isOpen(uint8_t id) const { return id < numValves && slots[id].open; }
    uint8_t stateBitmap() const;
    void setBatteryLockout(bool locked) { batteryLockout = locked; }

  private:
    struct Slot {
        bool open = false;
        uint32_t closeAtMs = 0;
    };
    IValveDriver &driver;
    uint8_t numValves;
    bool batteryLockout = false;
    Slot slots[MAX_VALVES];
};
