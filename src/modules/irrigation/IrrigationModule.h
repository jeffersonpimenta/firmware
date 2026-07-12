#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "modules/irrigation/Allowlist.h"
#include "modules/irrigation/ButtonGesture.h"
#include "modules/irrigation/FragmentReassembler.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "modules/irrigation/IrrigationSettings.h"
#include "modules/irrigation/LedPattern.h"
#include "modules/irrigation/Pairing.h"
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

    // Called by IrrigationUiThread (25 ms poll) to dispatch button gestures.
    void onButtonEvent(ButtonGestureDetector::Event ev);
    // Called by IrrigationUiThread to read current LED state (pure function of time).
    bool ledOnNow() { return led.ledOn(millis()); }

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    void handleCmdValvula(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleSetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    // Pairing handlers — no senderAuthorized check; physical window + button is the authorization (spec §6).
    void handlePairAnnounce(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handlePairGrant(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void commitPairing();
    void factoryReset();
    void logFarmKey(); // dumps primary PSK as base64 to serial (spec §11.2)
    void sendEvento(uint8_t code, uint32_t arg = 0);
    void refreshLedMode(); // call at end of runOnce
    bool loadAllowlist();
    bool saveAllowlist();
    IrrigationSettings mergeRemoteConfig(const IrrigationSettings &fresh, uint32_t newEpoch) const;
    void activateSettings(const IrrigationSettings &merged);
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
    FragmentReassembler reasm;
    StationPairing stationPairing;
    GatewayPairing gatewayPairing;
    Allowlist allowlist;
    LedPatternController led;
    uint32_t txSeq = 0;
    uint32_t lastHeartbeatMs = 0;
    uint32_t lastGatewayRxMs = 0; // last millis() we received a packet from boundGateway
    bool safeMode = false;
    bool bootHeartbeatPending = true;
};

extern IrrigationModule *irrigationModule;
