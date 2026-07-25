#pragma once
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "modules/irrigation/Allowlist.h"
#include "modules/irrigation/AuditLog.h"
#include "modules/irrigation/ButtonGesture.h"
#include "modules/irrigation/FlashAuditRing.h"
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
#include "modules/irrigation/LittleFsByteStore.h"
#include "modules/irrigation/ServiceController.h"
#include "modules/irrigation/ServicePortalApi.h" // Fase 8c: tipos/builders do portal SERVICO
#include "modules/irrigation/LittleFsProfileStore.h"

// Forward-decl da cola web (definida em IrrigationWebApi.h, incluída só no .cpp).
namespace IrrigationWeb
{
struct WebCommand;
struct NodeStateCtx;
struct PortalPulseReq;
struct NetCommand;
struct PortalSensorsCtx;
struct PortalGpoReq;
struct PortalCoords;
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
    // Fase 6b, Task 14b: remonta e empurra regras locais de intertravamento para cada estação.
    // Deve ser chamado após loadInterlocks()+loadGatewayState() (init) e após CRUD de interlocks (Task 18).
    void gwRebuildLocalInterlocks(); // GATEWAY-only
    // Fase 6b Task 18: métodos de alto nível para os endpoints de intertravamentos/sensores/manutenção.
    // Espelham o padrão gwApplyZone*/gwApplyProgram*: mutam, persistem e retornam false se falhar.
    bool gwApplyInterlockUpsert(const InterlockRule &r); // upsert + saveInterlocks() + gwRebuildLocalInterlocks()
    bool gwApplyInterlockDelete(uint8_t id);             // removeById + saveInterlocks() + gwRebuildLocalInterlocks()
    // Fase 7b: grupos hidráulicos (painel). Espelham gwApplyInterlock*.
    bool gwApplyGroupUpsert(HydraulicGroup &g, char *err, size_t errCap); // valida + aloca id se 0 + saveGroups()
    bool gwApplyGroupDelete(uint8_t id);                                  // removeById + saveGroups()
    bool gwRunGroupCommand(uint8_t id, bool open, uint16_t durationS);    // controle manual do grupo
    bool gwApplySensorName(uint32_t node, uint8_t sensorIdx, const char *name); // set + saveSensorNames()
    void gwOpenMaintWindow(uint32_t node, uint16_t minutes); // chama gwSendMaintWindow() privado
    // Acesso de leitura ao log de auditoria flash do gateway (Task 16). GATEWAY-only.
    const FlashAuditRing &auditFlashRef() const { return auditFlash; }
    // Fase 8b: backup §5.5 completo (PSK + tabelas) num envelope de 1 cliente, p/ o cofre
    // do device SERVICO / botão de export do painel. GATEWAY-only. → bytes escritos (0 se falhou).
    size_t gwBuildBackup(char *buf, size_t cap);

    // --- Serviço do portal de campo (todos os papéis). Chamados pela cola HTTP (IrrigationPortalEndpoints). ---
    void portalFillNodeState(IrrigationWeb::NodeStateCtx &out) const;
    bool portalPulse(const IrrigationWeb::PortalPulseReq &p);
    bool portalRunNetCommand(const IrrigationWeb::NetCommand &c);
    void portalFillSensors(IrrigationWeb::PortalSensorsCtx &out) const;
    bool portalGpo(const IrrigationWeb::PortalGpoReq &r); // biestável (durationS==0 ao ligar) exige confirm
    void portalGetCoords(IrrigationWeb::PortalCoords &out) const;
    bool portalSetCoords(const IrrigationWeb::PortalCoords &c); // persiste settings (coordenada é local, não mexe em epoch)
    PortalSession &portalSession() { return portal; }
    // Acesso de leitura ao mini-log de auditoria (Task 10: portal de campo). §8.9
    const AuditLog &auditLogRef() const { return audit; }

    // Fase 8c — portal do device SERVICO (§11.8). Chamados pelos endpoints /api/portal/service/*.
    bool svcIsService() const;
    size_t svcPortalListClients(char *buf, size_t cap);
    bool svcPortalSelect(const char *id); // planRetune + applyRetune (re-tune + reboot)
    void svcPortalStartScan();
    size_t svcPortalScanResults(char *buf, size_t cap);
    bool svcPortalReadConfig(uint32_t node);  // envia GET_CONFIG direto
    bool svcPortalConfigReady(uint32_t node); // reply remontado pronto?
    size_t svcPortalGetReadConfig(char *buf, size_t cap);
    bool svcPortalWriteConfig(const IrrigationWeb::NodeConfigReq &req); // rota DIRECT (§11.6); VIA_GATEWAY=follow-on
    bool svcPortalNodeAction(const IrrigationWeb::NodeAction &a);       // pulso/zona/resync; approve_pair=follow-on
    size_t svcPortalBuildLog(char *buf, size_t cap);
    uint32_t gwTimeAdopted() const { return 0; } // TODO banca: ts adotado do gateway (HB/ACK); 0=sem RTC

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

    // Fase 6b Task 16: log de auditoria persistente em flash no gateway (~5 meses, 256 KB).
    // Somente usado quando role == GATEWAY; inicializado em loadGatewayState().
    static constexpr size_t AUDIT_FLASH_CAP = 16384; // slots
    static constexpr size_t AUDIT_FLASH_SIZE = FlashAuditRing::HEADER + AUDIT_FLASH_CAP * FlashAuditRing::REC;
    LittleFsByteStore auditFlashStore{"/prefs/irrigation_audit.dat", AUDIT_FLASH_SIZE};
    FlashAuditRing auditFlash{auditFlashStore};

    void handleCmdValvula(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleCmdGpo(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleCmdMaint(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleSetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    // Pairing handlers — no senderAuthorized check; physical window + button is the authorization (spec §6).
    void handlePairAnnounce(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handlePairGrant(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    // Gateway handlers (Fase 4, Task 6: decisões 1-6)
    void handleRemoteCmd(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleResyncSeq(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);   // §11.5 responder
    void handlePingSurvey(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);  // §11.4 responder (todos papéis)
    // Fase 8b — device SERVICO (role == SERVICO). Executores das intents do ServiceController.
    void applyRetune(const char *clientId);   // §11.3 re-tune do canal + reboot
    void svcEmitProbe();                       // §11.4 emissor da sonda PING_SURVEY (broadcast, FROM_SERVICE)
    void svcSendResyncRequest(uint32_t node);  // §11.5 RESYNC_SEQ REQUEST (FROM_SERVICE)
    void svcExportToConsole();                 // §11.7 despeja o envelope do cofre no serial (bancada)
    void handleGwAck(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGwHeartbeat(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGwEvento(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void handleGwSetConfig(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    void gwTick(); // chamado no runOnce do GATEWAY 1×/s
    // Fonte única de hora local do gateway: true + segundos-de-epoch local se há RTC válido; false caso contrário.
    bool computeLocalSecs(uint32_t &out) const;
    uint32_t gwSendValveCmd(uint32_t node, uint8_t index, uint8_t tipo, uint8_t action, uint16_t durationS,
                            uint8_t zoneId, uint8_t attempts); // decisão §1 — retorna o txSeq usado (0 se falhou)
    // Fase 7b: ponto único de decisão de roteamento de open/close de UMA zona.
    // Zona de grupo hidráulico => motor (setDesired); zona livre => false (caminho atual).
    bool routeZoneToGroup(uint8_t zoneId, bool open, uint16_t durationS);
    void gwSendMaintWindow(uint32_t node, uint16_t minutes); // Fase 6b Task 15: janela de manutenção do tamper
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
    bool loadInterlocks();      // Fase 6b: carrega tabela de intertravamentos do flash
    bool saveInterlocks();      // Fase 6b: persiste tabela de intertravamentos (staged-write)
    bool loadSensorNames();     // Fase 6b Task 18: carrega nomes de sensores do flash
    bool saveSensorNames();     // Fase 6b Task 18: persiste nomes de sensores (staged-write)
    bool loadGroups();          // Fase 7a: carrega tabela de grupos hidráulicos do flash
    bool saveGroups();          // Fase 7a: persiste tabela de grupos hidráulicos (staged-write)
    IrrigationSettings mergeRemoteConfig(const IrrigationSettings &fresh, uint32_t newEpoch) const;
    void activateSettings(const IrrigationSettings &merged);
    void sendAck(uint32_t to, uint32_t ackedSeq, uint8_t status, uint8_t reason);
    void sendHeartbeat();
    bool senderAuthorized(uint32_t from, uint16_t flags) const;
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
    // Fase 6b: bitmask de zonas já fechadas por intertravamento (borda de subida).
    // Indexado por zoneId (0..255); usa array compacto de 32 bytes (256 bits).
    uint8_t interlockClosedMask[32] = {0};
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
    // Fase 8b — device SERVICO: cofre + controlador. Alocados só quando role == SERVICO.
    LittleFsProfileStore *svcStore = nullptr;
    ServiceController *svc = nullptr;
    // Fase 8c — leitura de config de um nó pelo device: GET_CONFIG → SET_CONFIG reply remontado
    // (cacheado, NÃO aplicado em si mesmo, ao contrário de handleSetConfig da estação).
    void handleSvcSetConfigReply(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h);
    uint32_t svcReadNode = 0;
    bool svcReadReady = false;
    IrrigationSettings svcReadBlob;
    // Fase 6b Task 14c: estado da réplica local de intertravamento (estação).
    // Latch por regra (>= MAX_LOCAL_INTERLOCKS entradas); prev-mask p/ borda de subida.
    bool localInterlockLatch[IrrigationSettings::MAX_LOCAL_INTERLOCKS] = {false};
    uint8_t localFecharValvPrev = 0;
    uint8_t localFecharGpoPrev = 0;
    // Tamper (§8.12): debounce próprio, fora dos 4 slots de sensor.
    bool tamperActive = false;
    bool tamperRawLast = false;
    uint32_t tamperRawSinceMs = 0;
    bool tamperInit = false;
    // Janela de manutenção do tamper (Fase 6b Task 15): suprime EV_TAMPER até este instante.
    // 0 = janela fechada. Combinado com portal.apShouldBeUp() por OR.
    uint32_t tamperMaintUntilMs = 0;
};

extern IrrigationModule *irrigationModule;
