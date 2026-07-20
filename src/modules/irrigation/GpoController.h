#pragma once
#include <stdint.h>

// Saída de nível (relé/MOSFET). Implementação GPIO fica no módulo; testes usam mock.
class IGpoDriver
{
  public:
    virtual ~IGpoDriver() = default;
    virtual void set(uint8_t index, bool on) = 0;
};

class GpoController
{
  public:
    static constexpr uint8_t MAX_GPO = 2;

    enum class Result : uint8_t { OK, INVALID_ID };

    GpoController(IGpoDriver &driver, uint8_t numGpos);

    // action: 1 = ligar, 0 = desligar. durationS==0 ao ligar = biestável (§8.11).
    Result command(uint8_t id, uint8_t action, uint16_t durationS, uint32_t nowMs);
    void tick(uint32_t nowMs); // desliga temporizados expirados
    void allOff();             // modo seguro / factory reset
    bool isOn(uint8_t id) const { return id < numGpos && slots[id].on; }
    uint8_t states() const;
    void setNumGpos(uint8_t n); // reconfig runtime: encolher desliga as removidas

  private:
    struct Slot {
        bool on = false;
        uint32_t offAtMs = 0; // 0 = biestável (sentinela)
    };
    IGpoDriver &driver;
    uint8_t numGpos;
    Slot slots[MAX_GPO];
};
