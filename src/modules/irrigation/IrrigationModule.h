#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "modules/irrigation/IrrigationSettings.h"
#include "modules/irrigation/RateLimiter.h"
#include "modules/irrigation/SeqTable.h"
#include "modules/irrigation/ValveController.h"

// Ponte H latching: pulso em A abre, pulso em B fecha.
class GpioValveDriver : public IValveDriver
{
  public:
    void configure(const IrrigationSettings &s);
    void pulse(uint8_t index, bool open) override;

  private:
    int8_t pinsA[IrrigationSettings::MAX_VALVES];
    int8_t pinsB[IrrigationSettings::MAX_VALVES];
    uint16_t pulseMs = 60;
};

class IrrigationModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    IrrigationModule();

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    void handleCmdValvula(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void sendAck(uint32_t to, uint32_t ackedSeq, uint8_t status, uint8_t reason);
    void sendHeartbeat();
    bool senderAuthorized(uint32_t from) const;
    uint16_t batteryCentiV() const;

    // settings must be declared before driver and valves so it is constructed
    // first and can feed the ValveController initializer (reference member).
    IrrigationSettings settings;
    GpioValveDriver driver;
    ValveController valves;
    SeqTable seqTable;
    RateLimiter rateLimiter;
    uint32_t txSeq = 0;
    uint32_t lastHeartbeatMs = 0;
};

extern IrrigationModule *irrigationModule;
