#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "modules/irrigation/Allowlist.h"
#include "modules/irrigation/AuditLog.h"
#include "modules/irrigation/ButtonGesture.h"
#include "modules/irrigation/FragmentReassembler.h"
#include "modules/irrigation/IrrigationGateway.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "modules/irrigation/IrrigationSettings.h"
#include "modules/irrigation/LedPattern.h"
#include "modules/irrigation/Pairing.h"
#include "modules/irrigation/RateLimiter.h"
#include "modules/irrigation/SeqTable.h"
#include "modules/irrigation/PortalSession.h"
#include "modules/irrigation/GpoController.h"
#include "modules/irrigation/ValveController.h"
#include "modules/irrigation/SensorSampler.h"

// Forward-decl da cola web (definida em IrrigationWebApi.h, incluída só no .cpp).
namespace IrrigationWeb
{
struct WebCommand;
struct NodeStateCtx;
struct PortalPulseReq;
struct NetCommand;
}

// Saída de nível (relé/MOSFET) dos GPOs.
class GpioGpoDriver : public IGpoDriver
{
  public:
    void configure(const IrrigationSettings &s);
    void set(uint8_t index, bool on) override;

  private:
    int8_t pins[IrrigationSettings::MAX_GPO] = {-1, -1};
};

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

    // --- Serviço do painel web (gateway). Chamados pela cola HTTP (IrrigationWebEndpoints). ---
    bool gwIsGateway() const;
    const IrrigationGateway &gwState() const { return gateway; }
    bool gwHasRtc() const;
    uint32_t gwLocalSecs() const;
    bool gwApplyZoneUpsert(const Zone &z);
    bool gwApplyZoneDelete(uint8_t id);
    bool gwApplyProgramUpsert(const Program &p);
    bool gwApplyProgramToggle(uint8_t id, bool enabled);
    bool gwApplyProgramDelete(uint8_t id);
    bool gwRunCommand(const IrrigationWeb::WebCommand &c);

    // --- Serviço do portal de campo (todos os papéis). Chamados pela cola HTTP (IrrigationPortalEndpoints). ---
    void portalFillNodeState(IrrigationWeb::NodeStateCtx &out) const;
    bool portalPulse(const IrrigationWeb::PortalPulseReq &p);
    bool portalRunNetCommand(const IrrigationWeb::NetCommand &c);
    PortalSession &portalSession() { return portal; }
    // Acesso de leitura ao mini-log de auditoria (Task 10: portal de campo). §8.9
    const AuditLog &auditLogRef() const { return audit; }

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    // §8.9: mini-log de auditoria da estação (100 registros em ring).
    static constexpr size_t AUDIT_CAP = 100;
    AuditRecord auditStore[AUDIT_CAP];
    AuditLog audit{auditStore, AUDIT_CAP};
    bool auditDirty = false;
    uint32_t lastAuditSaveMs = 0;
    void auditEvent(AuditOrigin o, AuditAction a, uint8_t target, AuditResult res, uint32_t node = 0, uint32_t seq = 0);
    bool loadAuditLog();
    bool saveAuditLog();

    void handleCmdValvula(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleCmdGpo(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleSetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    // Pairing handlers — no senderAuthorized check; physical window + button is the authorization (spec §6).
    void handlePairAnnounce(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handlePairGrant(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    // Gateway handlers (Fase 4, Task 6: decisões 1-6)
    void handleRemoteCmd(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGwAck(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGwHeartbeat(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGwEvento(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGwSetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void gwTick(); // chamado no runOnce do GATEWAY 1×/s
    // Fonte única de hora local do gateway: true + segundos-de-epoch local se há RTC válido; false caso contrário.
    bool computeLocalSecs(uint32_t &out) const;
    void gwSendValveCmd(uint32_t node, uint8_t index, uint8_t tipo, uint8_t action, uint16_t durationS, uint8_t zoneId,
                        uint8_t attempts); // decisão §1
    void gwReconcileEpoch(uint32_t node, uint32_t remoteEpoch);
    void commitPairing();
    void factoryReset();
    void logFarmKey(); // dumps primary PSK as base64 to serial (spec §11.2)
    void sendEvento(uint8_t code, uint32_t arg = 0);
    void refreshLedMode(); // call at end of runOnce
    void tickTamper(uint32_t nowMs);
    void resetTamperState();
    void configureSensorPins();
    bool loadAllowlist();
    bool saveAllowlist();
    bool loadGatewayState();
    bool saveGatewayState(); // persiste stations, zones, programs, mirror
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
    GpioGpoDriver gpoDriver;
    GpoController gpos;
    SeqTable seqTable;
    RateLimiter rateLimiter;
    FragmentReassembler reasm;
    StationPairing stationPairing;
    GatewayPairing gatewayPairing;
    Allowlist allowlist;
    LedPatternController led;
    PortalSession portal; // ciclo de vida do AP do captive portal (Fase 5b)
    // Gateway aggregate — only meaningful when role == GATEWAY (Task 6, decisão §1).
    IrrigationGateway gateway;
    // Cooldowns de reconciliação de epoch indexados por nó (Fix 3: node-keyed, não por posição na allowlist).
    struct EpochCooldown {
        uint32_t node = 0;
        uint32_t lastMs = 0;
    };
    EpochCooldown epochCooldowns[StationRegistry::MAX];
    uint32_t txSeq = 0;
    uint32_t lastHeartbeatMs = 0;
    uint32_t lastGatewayRxMs = 0; // last millis() we received a packet from boundGateway
    bool safeMode = false;
    bool bootHeartbeatPending = true;
    // Controle de LOG_WARN de RTC (1×/h para não spam)
    uint32_t lastRtcWarnMs = 0;
    // Marcador de "reconhecimento" de alertas (Fase 5a): alertas com atMs <= este valor são
    // considerados reconhecidos pelo overview. ACK_ALERT seta = millis().
    uint32_t lastAckAllMs = 0;
    SensorSampler sampler;
    // Tamper (§8.12): debounce próprio, fora dos 4 slots de sensor.
    bool tamperActive = false;
    bool tamperRawLast = false;
    uint32_t tamperRawSinceMs = 0;
    bool tamperInit = false;
};

extern IrrigationModule *irrigationModule;
