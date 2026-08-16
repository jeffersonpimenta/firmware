#include "modules/irrigation/IrrigationModule.h"
#include "FSCommon.h"
#include "airtime.h"
#include "modules/irrigation/IrrigationAirtime.h"
#include "modules/irrigation/AccessWindowPolicy.h"
#include "modules/irrigation/IrrigationWebApi.h"
#include "modules/irrigation/PortalApi.h"
#include "modules/irrigation/WeatherEngine.h"
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER
#include "modules/irrigation/PortalAp.h"
#endif
#if defined(ARCH_ESP32)
#include "platform/esp32/MeshtasticOTA.h"
#endif
#include "MeshService.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "Throttle.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "modules/irrigation/IrrigationBoardDefaults.h"
#include "modules/irrigation/RemoteButtonEdge.h"
#include "main.h"
#include "mesh/Channels.h"
#include "mesh/wifi/WiFiAPClient.h" // Fase 8b: triggerNtpUpdate/ntpLastRunMs (free functions)
#include <string.h>

// Default manual open duration when the user double-presses the station button (spec §5.2).
static constexpr uint32_t DEFAULT_MANUAL_OPEN_S = 20 * 60; // 20 min

static const char *ALLOWLIST_PATH = "/prefs/irrigation-allow.dat";
static const char *ALLOWLIST_TMP = "/prefs/irrigation-allow.tmp";

// Persistência do mini-log de auditoria (§8.9) — staged-write.
static const char *AUDIT_LOG_PATH = "/prefs/irrigation_log.dat";
static const char *AUDIT_LOG_TMP = "/prefs/irrigation_log.tmp";

// Persistência do estado gateway (Task 6, decisão §5) — staged-write em 4 arquivos.
static const char *GW_STATIONS_PATH = "/prefs/irrigation-stations.dat";
static const char *GW_STATIONS_TMP = "/prefs/irrigation-stations.tmp";
static const char *GW_ZONES_PATH = "/prefs/irrigation-zones.dat";
static const char *GW_ZONES_TMP = "/prefs/irrigation-zones.tmp";
static const char *GW_PROGRAMS_PATH = "/prefs/irrigation-programs.dat";
static const char *GW_PROGRAMS_TMP = "/prefs/irrigation-programs.tmp";
static const char *GW_MIRROR_PATH = "/prefs/irrigation-mirror.dat";
static const char *GW_MIRROR_TMP = "/prefs/irrigation-mirror.tmp";
// Fase 6b: tabela de intertravamentos (arquivo separado — não mistura com o estado do scheduler).
static const char *GW_INTERLOCKS_PATH = "/prefs/irrigation_interlocks.dat";
static const char *GW_INTERLOCKS_TMP = "/prefs/irrigation_interlocks.tmp";
// Fase 6b Task 18: nomes de sensores configurados pelo operador via painel.
static const char *GW_SENSORNAMES_PATH = "/prefs/irrigation_sensornames.dat";
static const char *GW_SENSORNAMES_TMP = "/prefs/irrigation_sensornames.tmp";
// Fase 7a: grupos hidráulicos (bomba/válvula).
static const char *GW_GRUPOS_PATH = "/prefs/irrigation_grupos.dat";
static const char *GW_GRUPOS_TMP = "/prefs/irrigation_grupos.tmp";
// controle de nível por boia (enchimento automático).
static const char *GW_NIVEIS_PATH = "/prefs/irrigation_niveis.dat";
static const char *GW_NIVEIS_TMP = "/prefs/irrigation_niveis.tmp";
// Supressão climática Open-Meteo (config + regras, arquivo separado por blob).
static const char *GW_WEATHERCFG_PATH = "/prefs/irrigation_weathercfg.dat";
static const char *GW_WEATHERCFG_TMP  = "/prefs/irrigation_weathercfg.tmp";
static const char *GW_WEATHERRULES_PATH = "/prefs/irrigation_weatherrules.dat";
static const char *GW_WEATHERRULES_TMP  = "/prefs/irrigation_weatherrules.tmp";
// Modo Remoto (Task 6): tabela de associações botoeira→saída.
static const char *GW_REMOTE_PATH = "/prefs/irrigation-remote.dat";
static const char *GW_REMOTE_TMP  = "/prefs/irrigation-remote.tmp";

// Cooldown de reconciliação de epoch por nó (30 s)
static constexpr uint32_t EPOCH_COOLDOWN_MS = 30000;

IrrigationModule *irrigationModule;

using namespace IrrigationProto;

namespace
{
// Leitor Arduino dos sensores; testes do SensorSampler usam mock próprio.
struct ArduinoSensorReader : ISensorReader {
    uint16_t readAdc(int8_t pin) override
    {
#ifndef ARCH_PORTDUINO
        return (uint16_t)analogRead(pin);
#else
        (void)pin;
        return 0;
#endif
    }
    bool readLevel(int8_t pin) override
    {
#ifndef ARCH_PORTDUINO
        return digitalRead(pin) != 0;
#else
        (void)pin;
        return false;
#endif
    }
};
ArduinoSensorReader sensorReader;
} // namespace

static IrrigationSettings loadIrrigationSettingsOrDefault()
{
    IrrigationSettings s;
    if (!loadIrrigationSettings(s))      // sem blob persistido = 1º boot (nenhum blob jamais gravado)
        applyBoardIrrigationDefaults(s); // aplica o mapa de pinos da board (no-op sem variant custom)
    if (s.pulseMs > 1000)
        s.pulseMs = 1000;
    return s;
}

void GpioValveDriver::configure(const IrrigationSettings &s)
{
    memcpy(pinsA, s.pinsHbridgeA, sizeof(pinsA));
    memcpy(pinsB, s.pinsHbridgeB, sizeof(pinsB));
    pulseMs = s.pulseMs;
#ifndef ARCH_PORTDUINO
    for (uint8_t i = 0; i < IrrigationSettings::MAX_VALVES; i++) {
        if (pinsA[i] >= 0) {
            pinMode(pinsA[i], OUTPUT);
            digitalWrite(pinsA[i], LOW);
        }
        if (pinsB[i] >= 0) {
            pinMode(pinsB[i], OUTPUT);
            digitalWrite(pinsB[i], LOW);
        }
    }
#endif
}

void GpioValveDriver::pulse(uint8_t index, bool open)
{
    int8_t pin = open ? pinsA[index] : pinsB[index];
    if (pin < 0) {
        LOG_WARN("Valve %d has no %s pin mapped", index, open ? "open" : "close");
        return;
    }
#ifndef ARCH_PORTDUINO
    digitalWrite(pin, HIGH);
    delay(pulseMs);
    digitalWrite(pin, LOW);
#endif
    LOG_INFO("Valve %d pulsed %s", index, open ? "OPEN" : "CLOSE");
}

// Conta quantos GPOs têm pino configurado (pinsGpo[i] >= 0).
static uint8_t countGpos(const IrrigationSettings &s)
{
    uint8_t n = 0;
    for (int i = 0; i < IrrigationSettings::MAX_GPO; i++)
        if (s.pinsGpo[i] >= 0)
            n++;
    return n;
}

void GpioGpoDriver::configure(const IrrigationSettings &s)
{
    memcpy(pins, s.pinsGpo, sizeof(pins));
#ifndef ARCH_PORTDUINO
    for (uint8_t i = 0; i < IrrigationSettings::MAX_GPO; i++) {
        if (pins[i] >= 0) {
            pinMode(pins[i], OUTPUT);
            digitalWrite(pins[i], LOW);
        }
    }
#endif
}

void GpioGpoDriver::set(uint8_t index, bool on)
{
    if (index >= IrrigationSettings::MAX_GPO)
        return;
    int8_t pin = pins[index];
    if (pin < 0)
        return;
#ifndef ARCH_PORTDUINO
    digitalWrite(pin, on ? HIGH : LOW);
#endif
    LOG_INFO("GPO %d %s", index, on ? "ON" : "OFF");
}

void IrrigationModule::configureSensorPins()
{
#ifndef ARCH_PORTDUINO
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        const auto &sl = settings.sensores[i];
        if (sl.pino < 0)
            continue;
        if (sl.tipo == 0) {
            // Digital: ativo-baixo usa pull-up interno
            pinMode(sl.pino, (sl.flags & 1) ? INPUT_PULLUP : INPUT);
        } else {
            // Analógico
            pinMode(sl.pino, INPUT);
        }
    }
    if (settings.pinTamper >= 0) {
        // bit0 de hwFlags = ativo-baixo: usa pull-up interno
        pinMode(settings.pinTamper, (settings.hwFlags & 1) ? INPUT_PULLUP : INPUT);
    }
#endif
}

// ---------------------------------------------------------------------------
// IrrigationUiThread — polls GPIO button at 25 ms, drives LED GPIO.
// Decision §4: only instantiated when pinBtn >= 0 || pinLed >= 0.
// In ARCH_PORTDUINO (native test environment) GPIO calls are compiled out.
// Defined before the IrrigationModule constructor that calls `new` on it.
// ---------------------------------------------------------------------------

class IrrigationUiThread : public concurrency::OSThread
{
  public:
    IrrigationUiThread(IrrigationModule *m, int8_t btnPin, int8_t ledPin)
        : OSThread("IrrigUi"), module(m), btn(btnPin), ledPin(ledPin)
    {
#ifndef ARCH_PORTDUINO
        if (btn >= 0)
            pinMode(btn, INPUT_PULLUP);
        if (ledPin >= 0)
            pinMode(ledPin, OUTPUT);
#endif
    }

  protected:
    int32_t runOnce() override
    {
        bool pressed = false;
#ifndef ARCH_PORTDUINO
        if (btn >= 0)
            pressed = (digitalRead(btn) == LOW);
#endif
        auto ev = detector.update(pressed, millis());
        if (ev != ButtonGestureDetector::Event::NONE)
            module->onButtonEvent(ev);
#ifndef ARCH_PORTDUINO
        if (ledPin >= 0)
            digitalWrite(ledPin, module->ledOnNow() ? HIGH : LOW);
#endif
        return 25; // 25 ms poll — fast enough for debounce (DEBOUNCE_MS=30)
    }

  private:
    IrrigationModule *module;
    int8_t btn, ledPin;
    ButtonGestureDetector detector;
};

IrrigationModule::IrrigationModule()
    : SinglePortModule("irrigation", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Irrigation"),
      settings(loadIrrigationSettingsOrDefault()), valves(driver, settings.numValves),
      gpos(gpoDriver, 0), rateLimiter(settings.cmdRatePerMin)
{
    IrrigationSettings probe;
    safeMode = !loadIrrigationSettings(probe); // sem config persistida = modo seguro (spec §5.5)
    provisioned = !safeMode;                   // sem config salva = nó de fábrica → wizard de 1º boot (§6)
    if (safeMode)
        gpos.allOff(); // §5.5 GPOs inativos em modo seguro
    driver.configure(settings);
    gpoDriver.configure(settings);
    gpos.setNumGpos(countGpos(settings));
    configureSensorPins();
    sampler.configure(settings);
    // Boot sempre em estado fechado (spec §5.5): solenoides latching podem ter
    // ficado abertos num reset com válvula acionada. forceCloseAll() pulsa
    // incondicionalmente, sem depender do bit open interno (que está falso
    // em todos os slots recém-criados).
    valves.forceCloseAll();
    // Gateway carrega sua allowlist de estações adotadas na memória (spec §6).
    // Task 6, decisão §5: carrega state do gateway (zones, stations, programs, mirror).
    if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY) {
        loadAllowlist();
        loadGatewayState();
        loadInterlocks();            // Fase 6b: carrega regras de intertravamento salvas
        loadSensorNames();           // Fase 6b Task 18: carrega nomes de sensores salvos
        loadGroups();                // Fase 7a: carrega grupos hidráulicos salvos
        loadLevels();                // controle de nível: carrega regras salvas
        loadWeather();               // config + regras de supressão climática
        loadRemoteButtons();         // Modo Remoto (Task 6): associações botoeira→saída
        gwRebuildLocalInterlocks();  // Fase 6b Task 14b: monta réplicas locais v5 e empurra via epoch
        // Fase 6b Task 16: inicializa o log de auditoria persistente em flash do gateway.
        auditFlashStore.ensureAllocated();
        auditFlash.begin();
    }
    // Fase 8b: device SERVICO — cofre LittleFS + controlador (§11).
    if ((IrrigationRole)settings.role == IrrigationRole::SERVICO) {
        svcStore = new LittleFsProfileStore("/clientes");
        svcStore->ensureDir();
        svc = new ServiceController(*svcStore);
    }
    // Telemetria nativa é redundante com o heartbeat de irrigação; alonga o intervalo
    // em RAM (não persiste) para desocupar airtime. Só quando não configurado pelo usuário.
    {
        IrrigationRole r = (IrrigationRole)settings.role;
        bool irrigRole = (r == IrrigationRole::ESTACAO || r == IrrigationRole::GATEWAY || r == IrrigationRole::REPETIDOR);
        if (irrigRole) {
            const uint32_t kLongIntervalS = 24u * 60u * 60u; // 24 h
            if (moduleConfig.telemetry.device_update_interval == 0 ||
                moduleConfig.telemetry.device_update_interval < kLongIntervalS)
                moduleConfig.telemetry.device_update_interval = kLongIntervalS;
            if (moduleConfig.telemetry.environment_update_interval != 0 &&
                moduleConfig.telemetry.environment_update_interval < kLongIntervalS)
                moduleConfig.telemetry.environment_update_interval = kLongIntervalS;
        }
    }
    // §8.9: carrega o mini-log sobrevivente de reboot e registra o boot.
    loadAuditLog();
    auditEvent(AuditOrigin::SISTEMA, AuditAction::REBOOT, /*target=*/0, AuditResult::OK);
    if (safeMode)
        auditEvent(AuditOrigin::SISTEMA, AuditAction::SAFE_MODE_IN, 0, AuditResult::OK);
    // §8.9: grava o registro de boot já — sem esperar o debounce de 60 s (útil p/ diagnosticar reboot loops).
    saveAuditLog();
    auditDirty = false;
    lastAuditSaveMs = millis();
    LOG_INFO("IrrigationModule role=%d valves=%d gateway=0x%08x epoch=%u safe=%d", settings.role, settings.numValves,
             settings.boundGateway, settings.configEpoch, (int)safeMode);
    // Spin up the GPIO button/LED thread only when at least one pin is wired (spec §8.6/§8.7).
    // The class is defined later in this translation unit; new'd here so it owns its own lifetime.
    if (settings.pinBtn >= 0 || settings.pinLed >= 0)
        new IrrigationUiThread(this, settings.pinBtn, settings.pinLed);
    // Janela de acesso (spec 2026-08-11): estação/repetidor provisionados abrem a
    // janela Portal AP + BLE no boot; ela fecha por inatividade e, na borda de
    // fechamento, o BLE é liberado de vez (ver runOnce). Fora do regime elegível
    // (fábrica/gateway/serviço) o comportamento antigo é preservado.
    if (AccessWindowPolicy::eligible((IrrigationRole)settings.role, provisioned))
        portal.requestOpen(millis());
}

bool IrrigationModule::wantPacket(const meshtastic_MeshPacket *p)
{
    if ((IrrigationRole)settings.role == IrrigationRole::REPETIDOR)
        return false; // repetidor só retransmite (spec §3.1)
    return p->decoded.portnum == ourPortNum;
}

bool IrrigationModule::senderAuthorized(uint32_t from, uint16_t flags) const
{
    // Predicado puro (§4.2, §11.5): marca de serviço OU não-vinculado OU vínculo com o remetente.
    // Sem vínculo gravado, qualquer nó do canal comanda (posse da PSK = autoridade, spec §4.2);
    // FLAG_FROM_SERVICE dispensa o vínculo para o nó de serviço (spec §11.5).
    return IrrigationProto::senderAuthorizedBy(flags, from, settings.boundGateway);
}

ProcessMessage IrrigationModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    const auto &d = mp.decoded;
    Header h;
    if (!decodeHeader(d.payload.bytes, d.payload.size, h)) {
        LOG_WARN("Irrigation: runt packet from 0x%08x", mp.from);
        return ProcessMessage::STOP;
    }
    if (h.version != VERSION) {
        LOG_WARN("Irrigation: protocol version %d != %d from 0x%08x", h.version, VERSION, mp.from);
        // NACK somente para pacotes diretos (não broadcast) e dentro do rate-limit
        // para evitar amplificação de NACKs em versões incompatíveis na rede.
        if (isToUs(&mp) && rateLimiter.allow(millis()))
            sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_VERSION);
        return ProcessMessage::STOP;
    }

    // Track liveness of the bound gateway for NO_GATEWAY LED heuristic (spec §8.7).
    if (settings.boundGateway != 0 && mp.from == settings.boundGateway) {
        lastGatewayRxMs = millis();
        noteGatewayLink((int8_t)(mp.rx_snr * 4), (int16_t)mp.rx_rssi);
    }

    switch (h.type) {
    case MSG_CMD_VALVULA:
        handleCmdValvula(mp, h);
        break;
    case MSG_CMD_GPO:
        handleCmdGpo(mp, h);
        break;
    case MSG_SET_CONFIG:
        // Task 6, decisão §3: role split — GATEWAY recebe SET_CONFIG como resposta de GET_CONFIG.
        // Fase 8c: SERVICO recebe SET_CONFIG como resposta do próprio GET_CONFIG (leitura de nó), sem aplicar em si.
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            handleGwSetConfig(mp, h);
        else if ((IrrigationRole)settings.role == IrrigationRole::SERVICO)
            handleSvcSetConfigReply(mp, h);
        else
            handleSetConfig(mp, h);
        break;
    case MSG_GET_CONFIG:
        handleGetConfig(mp, h);
        break;
    case MSG_PAIR_ANNOUNCE:
        handlePairAnnounce(mp, h);
        break;
    case MSG_PAIR_GRANT:
        handlePairGrant(mp, h);
        break;
    case MSG_RESYNC_SEQ:
        handleResyncSeq(mp, h);
        break;
    case MSG_PING_SURVEY:
        handlePingSurvey(mp, h);
        break;
    case MSG_ACK:
        // Task 6, decisão §3: gateway trata ACKs das estações.
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            handleGwAck(mp, h);
        else {
            // Estação: trata ACK de comando direto P2P (fallback) para acionar LED de confirmação.
            IrrigationProto::Ack ack;
            if (decodeAck(mp.decoded.payload.bytes, mp.decoded.payload.size, ack)) {
                for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
                    if (_pendingDirect[i].armed && _pendingDirect[i].seq == ack.ackedSeq) {
                        bool ok = (ack.status == IrrigationProto::ACK_OK);
                        uint8_t slot = _pendingDirect[i].ledSlot;
                        if (slot <= 1)
                            _remoteLed[slot].onLedState(ok, millis()); // SOLID no OK, OFF no NACK
                        _pendingDirect[i].armed = false;
                    }
                }
            } else {
                LOG_DEBUG("Irrigation: ACK from 0x%08x ignored (role=%d)", mp.from, settings.role);
            }
        }
        break;
    case MSG_HEARTBEAT:
        // Task 6, decisão §3: gateway trata HBs das estações.
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            handleGwHeartbeat(mp, h);
        else
            LOG_DEBUG("Irrigation: HB from 0x%08x ignored (role=%d)", mp.from, settings.role);
        break;
    case MSG_EVENTO:
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            handleGwEvento(mp, h);
        else
            LOG_DEBUG("Irrigation: EVENTO from 0x%08x ignored (role=%d)", mp.from, settings.role);
        break;
    case MSG_REMOTE_CMD:
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            handleRemoteCmd(mp, h);
        else
            LOG_DEBUG("Irrigation: REMOTE_CMD from 0x%08x ignored (role=%d)", mp.from, settings.role);
        break;
    case MSG_REMOTE_TRIGGER:
        if (settings.role == (uint8_t)IrrigationRole::GATEWAY)
            handleRemoteTrigger(mp, h);
        else
            LOG_DEBUG("Irrigation: REMOTE_TRIGGER from 0x%08x ignored (role=%d)", mp.from, settings.role);
        break;
    case MSG_REMOTE_LED:
        handleRemoteLed(mp, h);
        break;
    case MSG_CMD_MAINT:
        // Estação recebe janela de manutenção do tamper enviada pelo gateway (Fase 6b Task 15).
        if ((IrrigationRole)settings.role == IrrigationRole::ESTACAO)
            handleCmdMaint(mp, h);
        else
            LOG_DEBUG("Irrigation: CMD_MAINT from 0x%08x ignored (role=%d)", mp.from, settings.role);
        break;
    default:
        LOG_DEBUG("Irrigation: unhandled type %d from 0x%08x", h.type, mp.from);
        break;
    }
    return ProcessMessage::STOP;
}

void IrrigationModule::handleCmdValvula(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from, h.flags)) {
        LOG_WARN("Irrigation: unauthorized cmd from 0x%08x", mp.from);
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_UNAUTHORIZED, AuditResult::NACK, mp.from, h.seq);
        return;
    }
    // Retransmissões legítimas devem usar seq novo; seq repetido = replay ou bug
    // no gateway. Comandos limitados por rate-limit consomem o seq intencionalmente.
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: replayed seq %u from 0x%08x", h.seq, mp.from);
        return; // replay: descarta em silêncio, não ACKa
    }
    if (!rateLimiter.allow(millis())) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_RATE_LIMIT);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_RATE_LIMIT, AuditResult::NACK, mp.from, h.seq);
        return;
    }

    CmdValvula cmd;
    if (!decodeCmdValvula(mp.decoded.payload.bytes, mp.decoded.payload.size, cmd)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_BAD_PAYLOAD, AuditResult::NACK, mp.from, h.seq);
        return;
    }

    // P2P fallback: TOGGLE resolvido no alvo — inverte a saída local.
    if (cmd.action == IrrigationProto::ACTION_TOGGLE)
        cmd.action = remoteToggleAction(valves.isOpen(cmd.valveId));

    if (safeMode && cmd.action == 1) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_SAFE_MODE);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_SAFE_MODE, AuditResult::NACK, mp.from, h.seq);
        return;
    }

    ValveController::Result r;
    if (cmd.action == 1)
        r = valves.open(cmd.valveId, cmd.durationS, settings.maxOpenConfigS, millis());
    else
        r = valves.close(cmd.valveId);

    uint8_t reason = REASON_NONE;
    switch (r) {
    case ValveController::Result::OK:
        break;
    case ValveController::Result::INVALID_ID:
    case ValveController::Result::ZERO_DURATION:
        reason = REASON_INVALID_ID;
        break;
    case ValveController::Result::BATTERY_LOW:
        reason = REASON_BATTERY_LOW;
        break;
    }
    bool cmdOk = (reason == REASON_NONE);
    sendAck(mp.from, h.seq, cmdOk ? ACK_OK : ACK_NACK, reason);
    // §8.9: audita comando de válvula recebido por rádio.
    auditEvent(AuditOrigin::PAINEL, cmd.action == 1 ? AuditAction::ABRIR : AuditAction::FECHAR,
               cmd.valveId, cmdOk ? AuditResult::OK : AuditResult::NACK, mp.from, h.seq);
}

void IrrigationModule::handleCmdGpo(const meshtastic_MeshPacket &mp, const Header &h)
{
    // Apenas remetentes autorizados podem acionar GPOs (§4.2).
    if (!senderAuthorized(mp.from, h.flags)) {
        LOG_WARN("Irrigation: GPO cmd não autorizado de 0x%08x", mp.from);
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_UNAUTHORIZED, AuditResult::NACK, mp.from, h.seq);
        return;
    }
    // Seq repetido = replay ou bug no gateway — descarta em silêncio.
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: GPO seq repetido %u de 0x%08x", h.seq, mp.from);
        return;
    }
    if (!rateLimiter.allow(millis())) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_RATE_LIMIT);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_RATE_LIMIT, AuditResult::NACK, mp.from, h.seq);
        return;
    }

    CmdGpo cmd;
    if (!decodeCmdGpo(mp.decoded.payload.bytes, mp.decoded.payload.size, cmd)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_BAD_PAYLOAD, AuditResult::NACK, mp.from, h.seq);
        return;
    }

    if (cmd.action == IrrigationProto::ACTION_TOGGLE)
        cmd.action = remoteToggleAction(gpos.isOn(cmd.gpoId));

    // Modo seguro: bloqueia ativação de saídas (§5.5). Desligar continua permitido.
    if (safeMode && cmd.action == 1) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_SAFE_MODE);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_SAFE_MODE, AuditResult::NACK, mp.from, h.seq);
        return;
    }

    GpoController::Result r = gpos.command(cmd.gpoId, cmd.action, cmd.durationS, millis());

    uint8_t reason = REASON_NONE;
    if (r == GpoController::Result::INVALID_ID)
        reason = REASON_INVALID_ID;
    bool cmdOk = (reason == REASON_NONE);
    sendAck(mp.from, h.seq, cmdOk ? ACK_OK : ACK_NACK, reason);
    // §8.9: audita comando GPO recebido por rádio.
    auditEvent(AuditOrigin::PAINEL, cmd.action == 1 ? AuditAction::GPO_ON : AuditAction::GPO_OFF,
               cmd.gpoId, cmdOk ? AuditResult::OK : AuditResult::NACK, mp.from, h.seq);
}

void IrrigationModule::handleCmdMaint(const meshtastic_MeshPacket &mp, const Header &h)
{
    // Apenas o gateway vinculado pode abrir janela de manutenção (mesma regra dos outros comandos).
    if (!senderAuthorized(mp.from, h.flags)) {
        LOG_WARN("Irrigation: CMD_MAINT não autorizado de 0x%08x", mp.from);
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_UNAUTHORIZED, AuditResult::NACK, mp.from, h.seq);
        return;
    }
    // Anti-replay: seq repetido descartado em silêncio.
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: CMD_MAINT seq repetido %u de 0x%08x", h.seq, mp.from);
        return;
    }
    CmdMaint cmd;
    if (!decodeCmdMaint(mp.decoded.payload.bytes, mp.decoded.payload.size, cmd)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_BAD_PAYLOAD, AuditResult::NACK, mp.from, h.seq);
        return;
    }
    // Aplica janela de manutenção: 0 minutos = fechar imediatamente.
    if (cmd.durationMin == 0) {
        tamperMaintUntilMs = 0;
        LOG_INFO("Irrigation: janela de manutenção do tamper fechada por 0x%08x", mp.from);
    } else {
        tamperMaintUntilMs = millis() + (uint32_t)cmd.durationMin * 60000UL;
        LOG_INFO("Irrigation: janela de manutenção do tamper aberta por %u min (por 0x%08x)", cmd.durationMin, mp.from);
    }
    sendAck(mp.from, h.seq, ACK_OK, REASON_NONE);
    // §8.9: audita abertura/fechamento de janela de manutenção (ação TAMPER + origem PAINEL).
    auditEvent(AuditOrigin::PAINEL, AuditAction::TAMPER, cmd.durationMin > 0 ? 1 : 0, AuditResult::OK, mp.from, h.seq);
}

void IrrigationModule::sendAck(uint32_t to, uint32_t ackedSeq, uint8_t status, uint8_t reason)
{
    Ack ack = {};
    ack.ackedSeq = ackedSeq;
    ack.status = status;
    ack.reason = reason;
    ack.valveStates = valves.stateBitmap();
    ack.gpoStates = gpos.states();
    ack.vbatCentiV = batteryCentiV();
    ack.configEpoch = settings.configEpoch;

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = to;
    p->decoded.payload.size = encodeAck(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, ack);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    txPacket(p);
}

void IrrigationModule::handleResyncSeq(const meshtastic_MeshPacket &mp, const Header &h)
{
    (void)h;
    ResyncSeq req;
    if (!decodeResyncSeq(mp.decoded.payload.bytes, mp.decoded.payload.size, req))
        return;
    if (req.kind != 0) { // REPLY: device SERVICO retoma sua numeração em lastSeq+1 (§11.5 requester)
        if (svc) {
            char cid[32];
            if (svc->getVault().activeId(cid, sizeof cid))
                svc->onResyncReply(cid, mp.from, req.lastSeq);
        }
        return;
    }
    // Query read-only: NÃO passa pelo gate anti-replay (§11.5 — o remetente pode ter contador defasado).
    if (!rateLimiter.allow(millis()))
        return;
    ResyncSeq reply = {};
    reply.kind = 1;
    reply.lastSeq = seqTable.lastSeq(mp.from);

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = mp.from;
    p->decoded.payload.size =
        (uint16_t)encodeResyncSeq(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, reply);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    txPacket(p);
}

void IrrigationModule::handlePingSurvey(const meshtastic_MeshPacket &mp, const Header &h)
{
    (void)h;
    PingSurvey req;
    if (!decodePingSurvey(mp.decoded.payload.bytes, mp.decoded.payload.size, req))
        return;
    if (req.kind == 2) { // BEACON — site survey §8.5: gateway loga, ninguém responde
        if (gwIsGateway()) {
            SurveyPoint sp{};
            sp.node = mp.from;
            sp.role = req.role;
            sp.vbatCentiV = req.vbatCentiV;
            sp.fwVersion = req.fwVersion;
            sp.latE7 = req.latE7;
            sp.lonE7 = req.lonE7;
            sp.hasCoord = (req.latE7 != 0 || req.lonE7 != 0);
            sp.snrQuarterDb = (int8_t)(mp.rx_snr * 4);
            sp.rssiDbm = (int16_t)mp.rx_rssi;
            sp.uptimeS = millis() / 1000;
            surveyLog.add(sp);
        }
        return;
    }
    if (req.kind != 0) { // REPLY: coletado pelo prober do device SERVICO (§11.4)
        if (svc) {
            ScanEntry e{};
            e.node = mp.from;
            e.role = req.role;
            e.epoch = req.configEpoch;
            e.vbatCentiV = req.vbatCentiV;
            e.fwVersion = req.fwVersion;
            e.lat = req.latE7;
            e.lon = req.lonE7;
            e.snrQuarterDb = (int8_t)(mp.rx_snr * 4);
            svc->onSurveyReply(e);
        }
        return;
    }
    // Sonda broadcast, resposta só nodeinfo: isenta de auth/seq, mas rate-limited p/ proteger airtime.
    if (!rateLimiter.allow(millis()))
        return;
    PingSurvey reply = {};
    reply.kind = 1;
    reply.role = settings.role;
    reply.configEpoch = settings.configEpoch;
    reply.vbatCentiV = batteryCentiV();
    reply.fwVersion = APP_FW_VERSION;
    reply.latE7 = settings.latE7;
    reply.lonE7 = settings.lonE7;

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = mp.from;
    p->decoded.payload.size =
        (uint16_t)encodePingSurvey(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, reply);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    txPacket(p);
}

bool IrrigationModule::txPacket(meshtastic_MeshPacket *p)
{
    // Tipo IrrigationProto vive em payload.bytes[1] (Header.type). arg do evento em bytes[8..].
    uint8_t type = p->decoded.payload.size > 1 ? p->decoded.payload.bytes[1] : 0;
    bool crit = false;
    if (type == IrrigationProto::MSG_EVENTO && p->decoded.payload.size > IrrigationProto::HEADER_LEN)
        crit = IrrigationAirtime::isCriticalEvent(p->decoded.payload.bytes[IrrigationProto::HEADER_LEN]);

    p->priority = (meshtastic_MeshPacket_Priority)IrrigationAirtime::priorityForType(type, crit);

    if (IrrigationAirtime::isGatedType(type) && airTime && !airTime->isTxAllowedChannelUtil(/*polite=*/true)) {
        LOG_DEBUG("Irrigation tx gated (type=%u, chUtil high)", type);
        packetPool.release(p);
        return false;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    return true;
}

void IrrigationModule::sendHeartbeat()
{
    Heartbeat hb = {};
    hb.valveStates = valves.stateBitmap();
    hb.gpoStates = gpos.states();
    hb.vbatCentiV = batteryCentiV();
    hb.configEpoch = settings.configEpoch;
    hb.flags = safeMode ? HB_FLAG_SAFE_MODE : 0;
    if (tamperActive)
        hb.flags |= HB_FLAG_TAMPER;
    IrrigationProto::SensorReading rs[IrrigationSettings::MAX_SENSORS];
    hb.sensorCount = (uint8_t)sampler.readings(rs);
    for (uint8_t i = 0; i < hb.sensorCount; i++)
        hb.sensors[i] = rs[i];

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = settings.boundGateway ? settings.boundGateway : NODENUM_BROADCAST;
    p->decoded.payload.size = encodeHeartbeat(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, hb);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    txPacket(p);
    sampler.noteReported(millis());
    LOG_DEBUG("Irrigation heartbeat sent, valves=0x%x vbat=%u cV", hb.valveStates, hb.vbatCentiV);
}

uint16_t IrrigationModule::batteryCentiV() const
{
    if (powerStatus && powerStatus->getHasBattery())
        return powerStatus->getBatteryVoltageMv() / 10;
    return 0;
}

void IrrigationModule::tickTamper(uint32_t nowMs)
{
    if (settings.pinTamper < 0)
        return;
    bool raw = sensorReader.readLevel(settings.pinTamper);
    // Aplica polaridade: ativo-baixo inverte o sinal
    bool ativo = (settings.hwFlags & 1) ? !raw : raw;

    // Debounce de 200 ms fixo (§8.12)
    static constexpr uint32_t TAMPER_DEBOUNCE_MS = 200;

    if (!tamperInit) {
        // Primeira amostra: inicializa estado sem emitir evento
        tamperRawLast = ativo;
        tamperRawSinceMs = nowMs;
        tamperActive = ativo;
        tamperInit = true;
        return;
    }

    if (ativo != tamperRawLast) {
        // Mudança de nível: reinicia janela de debounce
        tamperRawLast = ativo;
        tamperRawSinceMs = nowMs;
        return;
    }

    // Nível estável — verifica se já passou o debounce e se mudou o estado confirmado
    if (ativo == tamperActive)
        return; // sem mudança
    if ((nowMs - tamperRawSinceMs) < TAMPER_DEBOUNCE_MS)
        return; // ainda dentro da janela

    bool novoEstado = ativo;
    tamperActive = novoEstado;
    // §8.9: tamper sempre auditado (mesmo quando EVENTO suprimido pela janela do portal ou manutenção — §8.12).
    auditEvent(AuditOrigin::SISTEMA, AuditAction::TAMPER, tamperActive ? 1 : 0, AuditResult::OK);
    // Portal aberto OU janela de manutenção ativa suprimem o EVENTO (§8.12); estado segue no heartbeat.
    bool suprimido = portal.apShouldBeUp() || (tamperMaintUntilMs != 0 && millis() < tamperMaintUntilMs);
    if (!suprimido)
        sendEvento(IrrigationProto::EV_TAMPER, tamperActive ? 1 : 0);
}

void IrrigationModule::resetTamperState()
{
    // Config nova pode trocar o pino/polaridade do tamper: reinicia o debounce
    // para que a primeira amostra inicialize sem disparar EV_TAMPER espúrio.
    tamperActive = false;
    tamperRawLast = false;
    tamperRawSinceMs = 0;
    tamperInit = false;
}

// Monta a config remota mesclada: identidade preservada, limites saneados.
IrrigationSettings IrrigationModule::mergeRemoteConfig(const IrrigationSettings &fresh, uint32_t newEpoch) const
{
    IrrigationSettings m = fresh;
    // role e boundGateway são identidade/credencial: config remota não toca (§5.5/§6)
    m.role = settings.role;
    m.boundGateway = settings.boundGateway;
    m.configEpoch = newEpoch;
    if (m.pulseMs > 1000)
        m.pulseMs = 1000;
    if (m.cmdRatePerMin == 0)
        m.cmdRatePerMin = 1; // 0 trancaria até o próprio SET_CONFIG de recuperação
    if (m.hbMinutes == 0)
        m.hbMinutes = 1;
    if (m.numValves > IrrigationSettings::MAX_VALVES)
        m.numValves = IrrigationSettings::MAX_VALVES;
    return m;
}

void IrrigationModule::activateSettings(const IrrigationSettings &merged)
{
    // Fecha tudo ANTES de trocar o pin map: os pulsos de fechar saem nos pinos antigos.
    valves.forceCloseAll();
    // GPOs desligam ANTES da troca do pin map: set(false) sai nos pinos antigos (mesma razão das válvulas).
    gpos.allOff();
    settings = merged;
    if (settings.numValves == 0)
        LOG_WARN("Irrigation: config sets zero valves");
    driver.configure(settings);
    valves.setNumValves(settings.numValves);
    gpoDriver.configure(settings);
    gpos.setNumGpos(countGpos(settings));
    configureSensorPins();
    sampler.configure(settings);
    resetTamperState();
    rateLimiter = RateLimiter(settings.cmdRatePerMin);
}

void IrrigationModule::handleSetConfig(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from, h.flags)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_UNAUTHORIZED, AuditResult::NACK, mp.from, h.seq);
        return;
    }
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: replayed config seq %u from 0x%08x", h.seq, mp.from);
        return; // replay silencioso: sem NACK, sem auditoria
    }
    if (!rateLimiter.allow(millis())) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_RATE_LIMIT);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_RATE_LIMIT, AuditResult::NACK, mp.from, h.seq);
        return;
    }

    SetConfig sc;
    if (!decodeSetConfig(mp.decoded.payload.bytes, mp.decoded.payload.size, sc)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_BAD_PAYLOAD, AuditResult::NACK, mp.from, h.seq);
        return;
    }

    auto r = reasm.add(mp.from, sc.epoch, sc.crc, sc.totalLen, sc.fragIndex, sc.fragCount, sc.frag, sc.fragLen, millis());
    switch (r) {
    case FragmentReassembler::Add::ACCEPTED:
        return; // fragmento intermediário: sem ACK (ACK = commit, §5.5)
    case FragmentReassembler::Add::TOO_BIG:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_CONFIG_TOO_BIG);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_CONFIG_TOO_BIG, AuditResult::NACK, mp.from, h.seq);
        return;
    case FragmentReassembler::Add::INVALID:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_FRAG_INVALID);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_FRAG_INVALID, AuditResult::NACK, mp.from, h.seq);
        return;
    case FragmentReassembler::Add::BAD_CRC:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_CRC);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_BAD_CRC, AuditResult::NACK, mp.from, h.seq);
        return;
    case FragmentReassembler::Add::COMPLETE:
        break;
    }

    IrrigationSettings fresh;
    if (!migrateIrrigationSettings(reasm.blob(), reasm.blobLen(), fresh)) {
        reasm.reset();
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_BAD_PAYLOAD, AuditResult::NACK, mp.from, h.seq);
        return;
    }
    uint32_t newEpoch = reasm.epoch();
    reasm.reset();

    IrrigationSettings merged = mergeRemoteConfig(fresh, newEpoch);
    if (!saveIrrigationSettings(merged)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_COMMIT_FAIL);
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, REASON_COMMIT_FAIL, AuditResult::NACK, mp.from, h.seq);
        return;
    }
    activateSettings(merged);
    bool wasSafe = safeMode;
    safeMode = false;
    LOG_INFO("Irrigation: config applied epoch=%u from 0x%08x", newEpoch, mp.from);
    sendAck(mp.from, h.seq, ACK_OK, REASON_NONE);
    // §8.9: audita adoção de epoch de configuração.
    auditEvent(AuditOrigin::PAINEL, AuditAction::CONFIG_EPOCH, 0, AuditResult::OK, mp.from, h.seq);
    if (wasSafe)
        auditEvent(AuditOrigin::SISTEMA, AuditAction::SAFE_MODE_OUT, 0, AuditResult::OK);
}

void IrrigationModule::handleGetConfig(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from, h.flags)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        return;
    }
    if (!seqTable.checkAndUpdate(mp.from, h.seq))
        return;
    if (!rateLimiter.allow(millis()))
        return; // leitura: sem NACK, requisitante re-tenta

    // Resposta = o blob vigente no formato SET_CONFIG (leitura não altera epoch, §11.6)
    const uint8_t *blob = (const uint8_t *)&settings;
    uint16_t totalLen = sizeof(settings);
    uint32_t crc = crc32(blob, totalLen);
    uint8_t fragCount = (uint8_t)((totalLen + FRAG_DATA_MAX - 1) / FRAG_DATA_MAX);
    for (uint8_t i = 0; i < fragCount; i++) {
        SetConfig sc = {};
        sc.epoch = settings.configEpoch;
        sc.crc = crc;
        sc.totalLen = totalLen;
        sc.fragIndex = i;
        sc.fragCount = fragCount;
        uint16_t off = (uint16_t)i * FRAG_DATA_MAX;
        sc.fragLen = (uint8_t)((totalLen - off > FRAG_DATA_MAX) ? FRAG_DATA_MAX : totalLen - off);
        sc.frag = blob + off;

        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = mp.from;
        p->decoded.payload.size =
            (uint16_t)encodeSetConfig(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, sc);
        if (!p->decoded.payload.size) {
            packetPool.release(p);
            return;
        }
        txPacket(p);
    }
}

int32_t IrrigationModule::runOnce()
{
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER
    portalApLoop(millis());
#endif
#ifdef ARCH_ESP32
    // Janela de acesso (spec 2026-08-11): detecta a borda OPEN->CLOSED da janela
    // num nó elegível e, uma única vez por boot, derruba o BLE de verdade
    // (config.bluetooth.enabled=false em RAM → deinit → release da RAM). O AP já
    // foi derrubado pelo próprio portalApLoop() quando apShouldBeUp() virou false.
    {
        bool eligible = AccessWindowPolicy::eligible((IrrigationRole)settings.role, provisioned);
        bool up = portal.apShouldBeUp();
        if (AccessWindowPolicy::shouldTearDownBle(eligible, up, apWasUp, bleReleasedThisBoot)) {
            LOG_INFO("Irrigation: access window closed — tearing down BLE (RAM release, sticky until reboot)");
            config.bluetooth.enabled = false; // RAM only — flash mantém enabled=true p/ o próximo boot.
            if (nimbleBluetooth)
                nimbleBluetooth->deinit();
            esp32ReleaseBluetoothMemoryIfUnused(); // shouldReleaseBluetoothMemory()==true agora
            bleReleasedThisBoot = true;
        }
        apWasUp = up;
    }
#endif
    // Fail-safe tick roda em TODOS os papéis: num nó mal-configurado nunca deve
    // sobrar válvula aberta sem timer sendo decrementado.
    // §8.9: captura estado antes do tick para detectar fechamentos por fail-safe timer.
    uint8_t vBefore = valves.stateBitmap();
    uint8_t gBefore = gpos.states();
    valves.tick(millis());
    gpos.tick(millis()); // §5.5 GPOs temporizados expirados são desligados automaticamente
    // Audita cada saída que foi fechada pelo fail-safe timer.
    uint8_t vAfter = valves.stateBitmap();
    uint8_t gAfter = gpos.states();
    for (uint8_t i = 0; i < IrrigationSettings::MAX_VALVES; i++) {
        if ((vBefore >> i & 1) && !(vAfter >> i & 1))
            auditEvent(AuditOrigin::FAILSAFE_TIMER, AuditAction::FECHAR, i, AuditResult::OK);
    }
    for (uint8_t i = 0; i < IrrigationSettings::MAX_GPO; i++) {
        if ((gBefore >> i & 1) && !(gAfter >> i & 1))
            auditEvent(AuditOrigin::FAILSAFE_TIMER, AuditAction::GPO_OFF, i, AuditResult::OK);
    }
    stationPairing.tick(millis());
    gatewayPairing.tick(millis());

    // Periodic PAIR_ANNOUNCE while the station pairing window is open (spec §6 step 1).
    // Must run before the early-return below so the role guard only needs to check ESTACAO.
    if ((IrrigationRole)settings.role == IrrigationRole::ESTACAO && stationPairing.announceDue(millis())) {
        PairAnnounce pa = {};
        pa.protoVersion = VERSION;
        // owner.short_name declared as extern meshtastic_User& in NodeDB.h; available via main.h include chain.
        pa.nameLen = (uint8_t)strnlen(owner.short_name, (sizeof(pa.name) - 1));
        memcpy(pa.name, owner.short_name, pa.nameLen);
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = NODENUM_BROADCAST;
        p->decoded.payload.size =
            (uint16_t)encodePairAnnounce(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, pa);
        if (p->decoded.payload.size)
            txPacket(p);
        else
            packetPool.release(p);
    }

    // §8.9: flush do mini-log com debounce de 60 s — preserva flash e sobrevive a reboot.
    // Roda antes do branch de papel para garantir flush em qualquer configuração.
    if (auditDirty && millis() - lastAuditSaveMs >= 60000) {
        saveAuditLog();
        auditDirty = false;
        lastAuditSaveMs = millis();
    }

    // Fase 8d — site survey (§8.5): emite beacon PING_SURVEY no intervalo, em qualquer papel que o iniciou.
    if (surveyBeacon.tick(millis()))
        emitSurveyBeacon();

    // --- Drive físico dos LEDs remotos — roda em TODOS os papéis (Task 7) ---
    // Gateway: onPress() acionado no Task 6 coloca _remoteLed em BLINK; precisa renderizar aqui.
    // Estação: recebe ACK do gateway e pisca confirmação ao operador.
    for (uint8_t slot = 0; slot <= 1; slot++) {
        if (settings.pinsRemoteLed[slot] < 0)
            continue;
#ifndef ARCH_PORTDUINO
        digitalWrite(settings.pinsRemoteLed[slot], _remoteLed[slot].ledOn(millis()) ? HIGH : LOW);
#endif
    }

    if ((IrrigationRole)settings.role != IrrigationRole::ESTACAO) {
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            gwTick(); // Task 6, decisão §2: loop principal do gateway 1×/s
        refreshLedMode();
        return 1000; // Task 6: cadência de 1 s (substitui 60 s/1 s condicional anterior)
    }
    uint16_t vbat = batteryCentiV();
    valves.setBatteryLockout(vbat != 0 && vbat < settings.vbatMinAbrirCentiV);

    sampler.tick(millis(), sensorReader);
    tickTamper(millis());

    // Fase 6b Task 14c: réplica local de intertravamento — fecha saídas SEM rádio.
    // Avalia todas as regras localInterlocks (preenchidas pelo gateway via SET_CONFIG)
    // e fecha na BORDA DE SUBIDA (bit 0→1) para não re-pulsar pontes H latching.
    {
        IrrigationProto::SensorReading rd[IrrigationSettings::MAX_SENSORS];
        size_t nrd = sampler.readings(rd);
        LocalReplicaOut lr = evalLocalInterlocks(settings.localInterlocks,
                                                 IrrigationSettings::MAX_LOCAL_INTERLOCKS,
                                                 rd, nrd,
                                                 localInterlockLatch);
        // Válvulas: fecha apenas as que passaram de 0→1 neste tick.
        uint8_t valvRising = lr.fecharValvMask & ~localFecharValvPrev;
        for (uint8_t i = 0; i < ValveController::MAX_VALVES; i++) {
            if (valvRising & (1u << i)) {
                valves.close(i);
                auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::FECHAR, i, AuditResult::OK);
            }
        }
        // GPOs: desliga apenas os que passaram de 0→1 neste tick (action 0 = off).
        uint8_t gpoRising = lr.fecharGpoMask & ~localFecharGpoPrev;
        for (uint8_t i = 0; i < GpoController::MAX_GPO; i++) {
            if (gpoRising & (1u << i)) {
                gpos.command(i, 0, 0, millis());
                auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::GPO_OFF, i, AuditResult::OK);
            }
        }
        localFecharValvPrev = lr.fecharValvMask;
        localFecharGpoPrev  = lr.fecharGpoMask;
    }

    if (sampler.earlyHeartbeatDue(millis()))
        sendHeartbeat(); // mudança significativa de sensor: reporte antecipado (rate-limited no sampler)

    if (bootHeartbeatPending && millis() > 10000) {
        bootHeartbeatPending = false;
        lastHeartbeatMs = millis();
        sendHeartbeat();
    }

    {
        uint32_t baseMs = (uint32_t)settings.hbMinutes * 60u * 1000u;
        float chUtil = airTime ? airTime->channelUtilizationPercent() : 0.0f;
        uint8_t factor = settings.hbBackoffEnabled() ? IrrigationAirtime::hbBackoffFactor(chUtil) : 1;
        uint32_t effectiveMs = baseMs * factor;

        // Re-semeia o jitter quando o epoch muda (descorrelaciona flood de cena).
        if (settings.configEpoch != hbSeedEpoch) {
            hbSeedEpoch = settings.configEpoch;
            uint32_t win = settings.hbJitterEnabled() ? IrrigationAirtime::hbWindowMs(effectiveMs) : 0;
            hbJitterMs = IrrigationAirtime::hbJitterOffsetMs(nodeDB->getNodeNum(), settings.configEpoch, win);
        }

        if (!Throttle::isWithinTimespanMs(lastHeartbeatMs, effectiveMs + hbJitterMs)) {
            lastHeartbeatMs = millis();
            sendHeartbeat();
        }
    }

    // --- Botoeira local da estação (Modo Remoto Task 7) ---
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
        if (settings.pinsDigitalIn[i] < 0 || !digitalInIsButton(settings, i))
            continue;
#ifndef ARCH_PORTDUINO
        bool raw = (digitalRead(settings.pinsDigitalIn[i]) == HIGH);
#else
        bool raw = false;
#endif
        bool activeLow = (settings.digitalInActiveLow >> i) & 1;
        bool pressed = activeLow ? !raw : raw;
        if (_btnEdge[i].update(pressed, millis())) {
            uint8_t slot = digitalInLedSlot(settings, i);
            if (slot <= 1)
                _remoteLed[slot].onPress(millis());
            if (settings.boundGateway != 0) {
                meshtastic_MeshPacket *p = allocDataPacket();
                p->to = settings.boundGateway;
                IrrigationProto::RemoteTrigger rt{i};
                p->decoded.payload.size = (uint16_t)IrrigationProto::encodeRemoteTrigger(
                    p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, rt);
                if (p->decoded.payload.size)
                    txPacket(p);
                else
                    packetPool.release(p);
            }
            if (settings.btnFallbackNode[i] != 0) {
                _fallback.setWindow(settings.remoteFallbackMs ? settings.remoteFallbackMs
                                                              : REMOTE_FALLBACK_DEFAULT_MS);
                _fallback.arm(i, millis());
            }
        }
    }

    // P2P fallback: gatilhos sem REMOTE_LED a tempo → comando direto ao nó alvo.
    {
        uint8_t exp[IrrigationSettings::MAX_DIGITAL_IN];
        size_t k = _fallback.takeExpired(millis(), exp, IrrigationSettings::MAX_DIGITAL_IN);
        for (size_t j = 0; j < k; j++) {
            uint8_t i = exp[j];
            if (settings.btnFallbackNode[i] == 0)
                continue;
            uint8_t kind = btnFallbackKindOf(settings, i); // 0=válvula,1=GPO
            meshtastic_MeshPacket *p = allocDataPacket();
            p->to = settings.btnFallbackNode[i];
            uint32_t seq = ++txSeq;
            if (kind == 1) {
                IrrigationProto::CmdGpo c{settings.btnFallbackOutId[i], IrrigationProto::ACTION_TOGGLE, 0};
                p->decoded.payload.size = (uint16_t)IrrigationProto::encodeCmdGpo(
                    p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), seq, c);
            } else {
                IrrigationProto::CmdValvula c{settings.btnFallbackOutId[i], IrrigationProto::ACTION_TOGGLE, 0};
                p->decoded.payload.size = (uint16_t)IrrigationProto::encodeCmdValvula(
                    p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), seq, c);
            }
            if (p->decoded.payload.size) {
                // Alvo pareado só aceita comando do gateway vinculado OU marcado como serviço
                // (senderAuthorizedBy §11.5). A botoeira não é o gateway, então carimba
                // FLAG_FROM_SERVICE — mesmo mecanismo que o gateway usa nos comandos dele.
                // A PSK do canal continua sendo a fronteira de confiança real.
                IrrigationProto::setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
                txPacket(p);
                uint8_t slot = digitalInLedSlot(settings, i);
                if (slot <= 1) {
                    _pendingDirect[i] = {seq, slot, true};
                }
            } else {
                packetPool.release(p);
            }
        }
    }

    refreshLedMode();
    return 1000; // tick de 1 s mantém o fail-safe responsivo
}

// ---------------------------------------------------------------------------
// Pairing handlers (spec §6). No senderAuthorized check — physical window +
// button press is the authorization. Both handlers apply anti-replay and rate
// limit at entry to prevent amplification attacks.
//
// adoção exige que o gateway mantenha um canal secundário com PSK default para
// escutar anúncios; o grant volta por esse canal (exposição aceita, spec §6).
// o modelo é UM botão por lado, não dois — qualquer anunciante durante a janela
// do gateway recebe a PSK; exposição aceita pela spec §6, não sobrestimar a garantia.
// ---------------------------------------------------------------------------

void IrrigationModule::handlePairAnnounce(const meshtastic_MeshPacket &mp, const Header &h)
{
    if ((IrrigationRole)settings.role != IrrigationRole::GATEWAY)
        return;
    // Anti-replay and rate-limit before any work (spec §4 — same guards as commands).
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: replayed announce seq %u from 0x%08x", h.seq, mp.from);
        return;
    }
    if (!rateLimiter.allow(millis()))
        return;
    PairAnnounce pa;
    if (!decodePairAnnounce(mp.decoded.payload.bytes, mp.decoded.payload.size, pa))
        return;
    if (!gatewayPairing.approveAnnounce(mp.from, millis())) {
        gatewayPairing.notePending(mp.from, millis()); // §6: painel mostra "nó novo detectado" p/ o operador aprovar
        LOG_INFO("Irrigation: announce from 0x%08x (%.*s) pending, window closed", mp.from, (int)pa.nameLen, pa.name);
        return;
    }
    gatewayPairing.clearPending(); // concedendo: o nó deixa de estar pendente
    // Decision §2: PSK must be exactly 32 bytes (own farm key); default/well-known PSK must not be granted.
    const meshtastic_ChannelSettings &prim = channels.getPrimary();
    if (prim.psk.size != 32) {
        LOG_WARN("Irrigation: cannot grant, primary channel has no 32-byte PSK (gateway needs its own farm key)");
        return;
    }
    PairGrant g = {};
    memcpy(g.psk, prim.psk.bytes, 32);
    const char *chName = channels.getName(channels.getPrimaryIndex());
    g.nameLen = (uint8_t)strnlen(chName, sizeof(g.channelName) - 1);
    memcpy(g.channelName, chName, g.nameLen);
    g.gatewayId = nodeDB->getNodeNum();

    if (!allowlist.add(mp.from)) {
        LOG_ERROR("Irrigation: allowlist full, refusing grant to 0x%08x", mp.from);
        return;
    }
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = mp.from;
    p->channel = mp.channel; // responde no canal do anúncio: o nó de fábrica não tem a PSK primária
    p->decoded.payload.size =
        (uint16_t)encodePairGrant(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, g);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        allowlist.remove(mp.from); // rollback: grant não foi enviado
        return;
    }
    txPacket(p);
    saveAllowlist();

    // Task 6, decisão §4: registrar a estação no StationRegistry no momento do pareamento.
    {
        StationEntry entry;
        entry.node = mp.from;
        entry.desiredEpoch = 0; // epoch 0 = defaults; será atualizado na primeira reconciliação
        uint8_t cpyLen = pa.nameLen < (uint8_t)(sizeof(entry.name) - 1) ? pa.nameLen : (uint8_t)(sizeof(entry.name) - 1);
        memcpy(entry.name, pa.name, cpyLen);
        entry.name[cpyLen] = '\0';
        if (gateway.stations.upsert(entry)) {
            saveGatewayState(); // persiste após mutação (decisão §5)
            LOG_INFO("Irrigation: station 0x%08x registered in gateway registry", mp.from);
        }
    }

    LOG_INFO("Irrigation: granted pairing to 0x%08x (%.*s)", mp.from, (int)pa.nameLen, pa.name);
}

void IrrigationModule::handlePairGrant(const meshtastic_MeshPacket &mp, const Header &h)
{
    if ((IrrigationRole)settings.role != IrrigationRole::ESTACAO)
        return;
    // Anti-replay and rate-limit at entry (spec §4).
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: replayed grant seq %u from 0x%08x", h.seq, mp.from);
        return;
    }
    if (!rateLimiter.allow(millis()))
        return;
    PairGrant g;
    if (!decodePairGrant(mp.decoded.payload.bytes, mp.decoded.payload.size, g))
        return;
    if (!stationPairing.onGrant(g, millis())) {
        LOG_WARN("Irrigation: grant from 0x%08x outside pairing window", mp.from);
        return;
    }
    commitPairing();
}

void IrrigationModule::commitPairing()
{
    const PairGrant &g = stationPairing.grant();
    // Decision §2: write farm PSK and channel name into the primary channel,
    // then schedule a reboot so all subsystems reload the new crypto context.
    // Adaptation: channels.getByIndex() returns a reference (confirmed in Channels.h line 40).
    meshtastic_Channel ch = channels.getByIndex(channels.getPrimaryIndex());
    memcpy(ch.settings.psk.bytes, g.psk, 32);
    ch.settings.psk.size = 32;
    memset(ch.settings.name, 0, sizeof(ch.settings.name));
    memcpy(ch.settings.name, g.channelName, g.nameLen);
    channels.setChannel(ch);
    channels.onConfigChanged();
    service->reloadConfig(SEGMENT_CHANNELS); // persiste o canal: sem isso, queda de
    // energia antes do reboot deixaria vínculo gravado com PSK antiga em flash

    uint32_t oldGw = settings.boundGateway;
    settings.boundGateway = g.gatewayId;
    if (!saveIrrigationSettings(settings)) {
        settings.boundGateway = oldGw;
        stationPairing.reset(); // permite reabrir a janela e re-parear sem power-cycle
        LOG_ERROR("Irrigation: pairing commit failed to persist");
        return;
    }
    safeMode = false;
    // §8.9: audita pareamento concluído com o gateway.
    auditEvent(AuditOrigin::SISTEMA, AuditAction::PAREAR, 0, AuditResult::OK, g.gatewayId);
    sendEvento(EV_PAIRED, g.gatewayId);
    LOG_INFO("Irrigation: paired to gateway 0x%08x, rebooting in 3 s", g.gatewayId);
    rebootAtMsec = millis() + 3000;
}

// Decision §3: factory reset clears irrigation prefs only; channel PSK remains
// (full credential removal requires the Phase-5 portal — documented limitation).
void IrrigationModule::factoryReset()
{
    // pulsos saem no pin map atual; depois do wipe os pinos viram -1 e nada mais fecha fisicamente.
    valves.forceCloseAll();
    gpos.allOff(); // §5.5 GPOs inativos em modo seguro
    LOG_WARN("Irrigation: factory reset by button");
    led.setMode(LedPatternController::Mode::PAIRING); // visual confirmation for 2 s before reboot
    sendEvento(EV_FACTORY_RESET); // entrega best-effort — reboot em 2 s pode cortar o TX
    IrrigationSettings def; // defaults: credenciais e parâmetros zerados
    def.role = settings.role; // papel e pinos são realidade física, não credencial
    def.numValves = settings.numValves;
    memcpy(def.pinsHbridgeA, settings.pinsHbridgeA, sizeof(def.pinsHbridgeA));
    memcpy(def.pinsHbridgeB, settings.pinsHbridgeB, sizeof(def.pinsHbridgeB));
    memcpy(def.pinsDigitalIn, settings.pinsDigitalIn, sizeof(def.pinsDigitalIn));
    def.digitalInActiveLow = settings.digitalInActiveLow;
    def.pinBtn = settings.pinBtn;
    def.pinLed = settings.pinLed;
    saveIrrigationSettings(def); // boundGateway=0, epoch=0: nó volta ao modo fábrica
#ifdef FSCom
    FSCom.remove(ALLOWLIST_PATH);
    // §8.9: factory reset apaga o log — estação volta zerada (decisão de design:
    // log de auditoria é parte da identidade do nó; reset deve limpar tudo).
    audit.clear();
    auditDirty = false; // reset já apagou o arquivo; não deixa o debounce gravar anel vazio.
    FSCom.remove(AUDIT_LOG_PATH);
#endif
    rebootAtMsec = millis() + 2000;
}

// Exports the primary channel name and PSK (base64) to the serial console for
// registration in the service device vault (spec §11.2). Physical access to the
// gateway is the authorization — same model as pairing. Phase 5 exposes this in
// the web portal (with PIN). Adaptation: inline base64 avoids a new dependency.
void IrrigationModule::logFarmKey()
{
    const meshtastic_ChannelSettings &prim = channels.getPrimary();
    if (prim.psk.size != 32) {
        LOG_WARN("Irrigation: primary channel has no 32-byte PSK to export");
        return;
    }
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[45];
    int o = 0;
    for (int i = 0; i < 32; i += 3) {
        uint32_t v = ((uint32_t)prim.psk.bytes[i] << 16) |
                     ((uint32_t)(i + 1 < 32 ? prim.psk.bytes[i + 1] : 0) << 8) |
                     (uint32_t)(i + 2 < 32 ? prim.psk.bytes[i + 2] : 0);
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = (i + 1 < 32) ? b64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < 32) ? b64[v & 63] : '=';
    }
    out[o] = '\0';
    LOG_INFO("Irrigation FARM KEY: channel=%s psk_b64=%s", channels.getName(channels.getPrimaryIndex()), out);
}

// Fase 8b — executores do device SERVICO (§11). Espelham commitPairing (re-tune) e o
// padrão allocDataPacket/encode/sendToMesh dos demais emissores.

void IrrigationModule::applyRetune(const char *clientId)
{
    if (!svc)
        return;
    RetunePlan r;
    if (!svc->planRetune(clientId, r)) {
        LOG_WARN("Irrigation SERVICO: retune plan failed for '%s'", clientId);
        return;
    }
    meshtastic_Channel ch = channels.getByIndex(channels.getPrimaryIndex());
    memset(ch.settings.psk.bytes, 0, sizeof(ch.settings.psk.bytes));
    memcpy(ch.settings.psk.bytes, r.psk, r.pskLen);
    ch.settings.psk.size = (uint16_t)r.pskLen;
    memset(ch.settings.name, 0, sizeof(ch.settings.name));
    strncpy(ch.settings.name, r.name, sizeof(ch.settings.name) - 1);
    channels.setChannel(ch);
    channels.onConfigChanged();
    config.lora.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)r.preset;
    config.lora.use_preset = true;
    // Persiste canal + config antes do reboot (§11.3): queda de energia não deixa estado meio-trocado.
    service->reloadConfig(SEGMENT_CHANNELS | SEGMENT_CONFIG);
    svc->getVault().select(clientId); // persiste ponteiro de cliente ativo (/clientes/active)
    LOG_INFO("Irrigation SERVICO: retune to client '%s' channel '%s', rebooting in 3 s", clientId, r.name);
    rebootAtMsec = millis() + 3000;
}

void IrrigationModule::svcEmitProbe()
{
    if (!svc)
        return;
    svc->clearScan();
    PingSurvey probe = {};
    probe.kind = 0; // PROBE
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    uint16_t sz =
        (uint16_t)encodePingSurvey(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, probe);
    if (!sz) {
        packetPool.release(p);
        return;
    }
    p->decoded.payload.size = sz;
    setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
    txPacket(p);
    LOG_INFO("Irrigation SERVICO: PING_SURVEY probe broadcast");
}

// Fase 8d — site survey (§8.5): beacon periódico de cobertura (kind=2), broadcast.
void IrrigationModule::emitSurveyBeacon()
{
    PingSurvey b = {};
    b.kind = 2; // BEACON
    b.role = settings.role;
    b.configEpoch = settings.configEpoch;
    b.vbatCentiV = batteryCentiV();
    b.fwVersion = APP_FW_VERSION;
    b.latE7 = surveyBeacon.hasCoord() ? surveyBeacon.latE7() : 0;
    b.lonE7 = surveyBeacon.hasCoord() ? surveyBeacon.lonE7() : 0;

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = NODENUM_BROADCAST;
    uint16_t sz = (uint16_t)encodePingSurvey(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, b);
    if (!sz) {
        packetPool.release(p);
        return;
    }
    p->decoded.payload.size = sz;
    if ((IrrigationRole)settings.role == IrrigationRole::SERVICO)
        setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
    txPacket(p);
}

bool IrrigationModule::portalStartSurvey(const IrrigationWeb::SurveyStartReq &r)
{
    surveyBeacon.start(millis(), r.intervalS, r.timeoutS, r.latE7, r.lonE7, r.hasCoord);
    return true;
}

void IrrigationModule::portalStopSurvey()
{
    surveyBeacon.stop();
}

size_t IrrigationModule::buildSurveyLog(char *buf, size_t cap)
{
    SurveyPoint tmp[SurveyLog::CAP];
    size_t k = surveyLog.count();
    for (size_t i = 0; i < k; i++)
        tmp[i] = surveyLog.at(i);
    return IrrigationWeb::buildSurvey(tmp, k, millis() / 1000, buf, cap);
}

void IrrigationModule::clearSurveyLog()
{
    surveyLog.clear();
}

void IrrigationModule::svcSendResyncRequest(uint32_t node)
{
    ResyncSeq req = {};
    req.kind = 0; // REQUEST
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = node;
    uint16_t sz = (uint16_t)encodeResyncSeq(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, req);
    if (!sz) {
        packetPool.release(p);
        return;
    }
    p->decoded.payload.size = sz;
    setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
    txPacket(p);
}

void IrrigationModule::svcExportToConsole()
{
    if (!svc)
        return;
    static char buf[4096];
    size_t n = svc->getVault().exportEnvelope(buf, sizeof buf);
    if (!n) {
        LOG_WARN("Irrigation SERVICO: vault export overflow");
        return;
    }
    buf[n < sizeof buf ? n : sizeof buf - 1] = 0;
    LOG_INFO("Irrigation SERVICO VAULT EXPORT: %s", buf);
}

// ── Fase 8c — portal do device SERVICO (§11.8): accessors dirigidos pelos endpoints ──

bool IrrigationModule::svcIsService() const
{
    return (IrrigationRole)settings.role == IrrigationRole::SERVICO;
}

size_t IrrigationModule::svcPortalListClients(char *buf, size_t cap)
{
    if (!svc)
        return 0;
    IrrigationService::LightProfile cs[16];
    size_t n = svc->getVault().listClients(cs, 16);
    char active[32] = {0};
    svc->getVault().activeId(active, sizeof active);
    return IrrigationWeb::buildClientList(cs, n, active, buf, cap);
}

bool IrrigationModule::svcPortalSelect(const char *id)
{
    if (!svc)
        return false;
    RetunePlan probe;
    if (!svc->planRetune(id, probe)) // valida existência + PSK antes de reiniciar
        return false;
    svc->logService("select", 0, gwTimeAdopted());
    applyRetune(id); // persiste ativo + reboot 3 s (§11.3)
    return true;
}

void IrrigationModule::svcPortalStartScan()
{
    if (!svc)
        return;
    svc->logService("scan", 0, gwTimeAdopted());
    svcEmitProbe(); // limpa scan + broadcast PROBE (8b)
}

size_t IrrigationModule::svcPortalScanResults(char *buf, size_t cap)
{
    if (!svc)
        return 0;
    return IrrigationWeb::buildScanResults(svc->scanResults(), buf, cap);
}

bool IrrigationModule::svcPortalReadConfig(uint32_t node)
{
    if (!svc || !node)
        return false;
    svcReadNode = node;
    svcReadReady = false;
    reasm.reset();
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = node;
    p->decoded.payload.size =
        (uint16_t)encodeGetConfig(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return false;
    }
    setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
    txPacket(p);
    svc->logService("get_config", node, gwTimeAdopted());
    return true;
}

void IrrigationModule::handleSvcSetConfigReply(const meshtastic_MeshPacket &mp, const Header &h)
{
    (void)h;
    if (mp.from != svcReadNode)
        return; // resposta de outro nó — ignora
    SetConfig sc;
    if (!decodeSetConfig(mp.decoded.payload.bytes, mp.decoded.payload.size, sc))
        return;
    auto r = reasm.add(mp.from, sc.epoch, sc.crc, sc.totalLen, sc.fragIndex, sc.fragCount, sc.frag, sc.fragLen, millis());
    if (r != FragmentReassembler::Add::COMPLETE)
        return;
    IrrigationSettings blob;
    if (migrateIrrigationSettings(reasm.blob(), reasm.blobLen(), blob)) {
        svcReadBlob = blob; // cacheia p/ o editor; NÃO aplica em si
        svcReadReady = true;
    }
    reasm.reset();
}

bool IrrigationModule::svcPortalConfigReady(uint32_t node)
{
    return svcReadReady && svcReadNode == node;
}

size_t IrrigationModule::svcPortalGetReadConfig(char *buf, size_t cap)
{
    if (!svcReadReady)
        return 0;
    return IrrigationWeb::buildStationConfig(svcReadBlob, buf, cap);
}

bool IrrigationModule::svcPortalWriteConfig(const IrrigationWeb::NodeConfigReq &req)
{
    if (!svc || !req.node)
        return false;
    if (req.route != IrrigationWeb::SvcRoute::DIRECT) {
        // VIA_GATEWAY exigiria um comando gateway-side "configurar estação X"; o wire atual não o tem
        // (SET_CONFIG ao gateway é adotado como config DELE). Follow-on; a rota direta converge via §5.4.
        svc->logService("set_config_gw_unsupported", req.node, gwTimeAdopted());
        return false;
    }
    // DIRECT (§11.6): grava na estação com epoch+1; o gateway adota depois pela regra do maior epoch (§5.4, 8a).
    IrrigationSettings blob = req.config;
    RouteDecision d = decideConfigRoute(false, req.config.configEpoch);
    blob.configEpoch = d.epochToWrite;
    // P2P fallback: sobrepõe a rota compilada nesta cópia local antes de calcular o CRC.
    gwOverlayFallbackRoute(req.node, blob);
    const uint8_t *raw = (const uint8_t *)&blob;
    uint16_t totalLen = (uint16_t)sizeof(IrrigationSettings);
    uint32_t crc = crc32(raw, totalLen);
    uint8_t fragCount = (uint8_t)((totalLen + FRAG_DATA_MAX - 1) / FRAG_DATA_MAX);
    for (uint8_t i = 0; i < fragCount; i++) {
        SetConfig sc = {};
        sc.epoch = blob.configEpoch;
        sc.crc = crc;
        sc.totalLen = totalLen;
        sc.fragIndex = i;
        sc.fragCount = fragCount;
        uint16_t off = (uint16_t)i * FRAG_DATA_MAX;
        sc.fragLen = (uint8_t)((totalLen - off > FRAG_DATA_MAX) ? FRAG_DATA_MAX : (uint8_t)(totalLen - off));
        sc.frag = raw + off;
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = req.node;
        p->decoded.payload.size =
            (uint16_t)encodeSetConfig(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, sc);
        if (!p->decoded.payload.size) {
            packetPool.release(p);
            return false;
        }
        setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
        txPacket(p);
    }
    svc->logService("set_config_direct", req.node, gwTimeAdopted());
    return true;
}

bool IrrigationModule::svcPortalNodeAction(const IrrigationWeb::NodeAction &a)
{
    if (!svc || !a.node)
        return false;
    switch (a.action) {
    case IrrigationWeb::SvcAction::PULSE: {
        CmdValvula cv{a.valveOrZoneId, 1, a.durationS}; // abrir por durationS (pulso)
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = a.node;
        p->decoded.payload.size =
            (uint16_t)encodeCmdValvula(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, cv);
        if (!p->decoded.payload.size) {
            packetPool.release(p);
            return false;
        }
        setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
        txPacket(p);
        svc->logService("pulse", a.node, gwTimeAdopted());
        return true;
    }
    case IrrigationWeb::SvcAction::ZONE: {
        RemoteCmd rc{a.valveOrZoneId, (uint8_t)(a.open ? 1 : 0), a.durationS};
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = a.node;
        p->decoded.payload.size =
            (uint16_t)encodeRemoteCmd(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, rc);
        if (!p->decoded.payload.size) {
            packetPool.release(p);
            return false;
        }
        setServiceFlag(p->decoded.payload.bytes, p->decoded.payload.size);
        txPacket(p);
        svc->logService("zone", a.node, gwTimeAdopted());
        return true;
    }
    case IrrigationWeb::SvcAction::RESYNC:
        svcSendResyncRequest(a.node); // 8b (carimba FLAG_FROM_SERVICE)
        svc->logService("resync", a.node, gwTimeAdopted());
        return true;
    case IrrigationWeb::SvcAction::APPROVE_PAIR:
    default:
        // Aprovação de pareamento pelo device (§11.8) exige intake device-side de PAIR_ANNOUNCE +
        // PairGrant com a PSK do cliente ativo — fluxo não presente na 8b. Follow-on.
        svc->logService("approve_pair_unsupported", a.node, gwTimeAdopted());
        return false;
    }
}

size_t IrrigationModule::svcPortalBuildLog(char *buf, size_t cap)
{
    if (!svcStore)
        return 0;
    return IrrigationWeb::buildServiceLog(svcStore->logReader(), 100, buf, cap);
}

size_t IrrigationModule::svcPortalExport(char *buf, size_t cap)
{
    if (!svc)
        return 0;
    return svc->getVault().exportEnvelope(buf, cap); // envelope multi-cliente (plaintext, §11.7)
}

bool IrrigationModule::svcPortalImport(const char *json, size_t n, bool replace, char *err, size_t errCap)
{
    if (!svc)
        return false;
    bool ok = svc->getVault().importEnvelope(json, n, replace, err, errCap); // valida em staging + merge por id
    if (ok)
        svc->logService(replace ? "import_replace" : "import_merge", 0, gwTimeAdopted());
    return ok;
}

bool IrrigationModule::svcPortalSeedConfig(uint32_t node, IrrigationSettings &out)
{
    if (!svcReadReady || svcReadNode != node)
        return false;
    out = svcReadBlob; // magic/version/role/boundGateway/configEpoch do blob lido → preservados no parse
    return true;
}

size_t IrrigationModule::gwBuildBackup(char *buf, size_t cap)
{
    if (!gwIsGateway())
        return 0;
    // Sub-arrays de config via os builders existentes do painel (DRY, Fase 5a-7b).
    static char zonasB[900], progB[800], interB[700], grupB[700], niveisB[512], remoteB[512];
    IrrigationWeb::buildZones(gateway.zones, zonasB, sizeof zonasB);
    IrrigationWeb::buildPrograms(gateway.scheduler, progB, sizeof progB);
    IrrigationWeb::buildInterlocks(gateway.interlocks, interB, sizeof interB);
    IrrigationWeb::buildGroups(gateway.groups, grupB, sizeof grupB);
    IrrigationWeb::buildLevelControls(gateway.levels, niveisB, sizeof niveisB);
    IrrigationWeb::buildRemote(remoteB, sizeof remoteB, gateway.remoteButtons, gateway.zones);
    // estacoes[] + snapshot_epoch{} a partir do registro de estações.
    static char estB[2048], epoB[600];
    {
        IrrigationWeb::JsonWriter w(estB, sizeof estB);
        w.beginArray();
        for (size_t i = 0; i < gateway.stations.count(); i++) {
            const StationEntry *s = gateway.stations.nodeAt(i);
            if (!s)
                continue;
            char no[12];
            snprintf(no, sizeof no, "!%08x", s->node);
            w.beginObject();
            w.keyStr("no", no);
            w.keyStr("nome", s->name);
            w.keyNum("lat", s->lat);
            w.keyNum("lon", s->lon);
            // Fase 9: config v6 desejada (heartbeat + limiares de bateria) p/ backup lossless.
            // Importador antigo ignora chaves desconhecidas (extractLight faz key-seek).
            IrrigationSettings cfg;
            if (s->desiredEpoch != 0 && migrateIrrigationSettings(s->blob, sizeof(s->blob), cfg)) {
                w.keyNum("hbMinutes", cfg.hbMinutes);
                w.keyNum("vbatAvisoCentiV", cfg.vbatAvisoCentiV);
                w.keyNum("vbatCriticaCentiV", cfg.vbatCriticaCentiV);
            }
            w.endObject();
        }
        w.endArray();
        w.done();
    }
    {
        IrrigationWeb::JsonWriter w(epoB, sizeof epoB);
        w.beginObject();
        for (size_t i = 0; i < gateway.stations.count(); i++) {
            const StationEntry *s = gateway.stations.nodeAt(i);
            if (!s)
                continue;
            char no[12];
            snprintf(no, sizeof no, "!%08x", s->node);
            w.keyNum(no, (int64_t)s->desiredEpoch);
        }
        w.endObject();
        w.done();
    }
    // PSK do canal primário → base64.
    const meshtastic_ChannelSettings &prim = channels.getPrimary();
    char pskB[48] = {0};
    if (prim.psk.size > 0)
        IrrigationService::base64Encode(prim.psk.bytes, prim.psk.size, pskB, sizeof pskB);
    const char *chName = channels.getName(channels.getPrimaryIndex());
    IrrigationService::BackupSource s{};
    s.id = "gateway"; // identidade default; o operador renomeia o cliente no cofre
    s.nome = chName;
    s.canalNome = chName;
    s.pskB64 = pskB;
    s.preset = (uint8_t)config.lora.modem_preset;
    s.gateway = nodeDB->getNodeNum();
    s.estacoesJson = estB;
    s.snapshotEpochJson = epoB;
    s.zonasJson = zonasB;
    s.programasJson = progB;
    s.intertravamentosJson = interB;
    s.gruposJson = grupB;
    s.sensorNamesJson = "[]"; // nomes de sensor: follow-up (sem builder dedicado)
    s.niveisJson = niveisB;
    s.remoteButtonsJson = remoteB;
    static char clientB[6144];
    size_t cn = IrrigationService::buildClientBackup(s, clientB, sizeof clientB);
    if (!cn)
        return 0;
    clientB[cn < sizeof clientB ? cn : sizeof clientB - 1] = 0;
    // Envelope multi-cliente de 1 cliente (formato de import do cofre §11.7).
    IrrigationWeb::JsonWriter w(buf, cap);
    w.beginObject();
    w.keyStr("fmt", "irrig-vault");
    w.keyNum("version", 1);
    w.key("clients");
    w.beginArray();
    w.raw(clientB);
    w.endArray();
    w.endObject();
    return w.done();
}

void IrrigationModule::sendEvento(uint8_t code, uint32_t arg)
{
    Evento ev = {code, arg};
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = settings.boundGateway ? settings.boundGateway : NODENUM_BROADCAST;
    p->decoded.payload.size =
        (uint16_t)encodeEvento(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, ev);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    txPacket(p);
}

// Janela de acesso (spec 2026-08-11): traduz o SHORT press num nó elegível.
// BLE vivo -> reabre a janela in-loco (portal + BLE seguem de pé). BLE já
// liberado (release sticky) -> só um reboot reabre a janela; agenda-o com LED de
// confirmação. Fora do regime elegível a política devolve NONE (no-op).
void IrrigationModule::applyAccessWindowShort()
{
    using Action = AccessWindowPolicy::ButtonAction;
    bool eligible = AccessWindowPolicy::eligible((IrrigationRole)settings.role, provisioned);
    switch (AccessWindowPolicy::buttonShortAction(eligible, bleReleasedThisBoot)) {
    case Action::REOPEN_LIVE:
        portal.requestOpen(millis());
        LOG_INFO("Irrigation: access window reopened (portal + BLE live)");
        break;
    case Action::REBOOT_TO_REOPEN:
        led.setMode(LedPatternController::Mode::PAIRING); // confirmação visual antes do reboot
        LOG_INFO("Irrigation: BLE released — rebooting in 1.5 s to reopen access window");
        rebootAtMsec = millis() + 1500;
        break;
    case Action::NONE:
    default:
        break;
    }
}

// Decision §6: button gestures per role/state.
void IrrigationModule::onButtonEvent(ButtonGestureDetector::Event ev)
{
    using Ev = ButtonGestureDetector::Event;
    IrrigationRole role = (IrrigationRole)settings.role;
    if (ev == Ev::HOLD_10S) { // factory reset valid in any role/state
        factoryReset();
        return;
    }
    if (role == IrrigationRole::ESTACAO && settings.boundGateway == 0) {
        // Factory node: any button event opens the pairing window (spec §8.6).
        stationPairing.openWindow(millis());
        refreshLedMode();
        LOG_INFO("Irrigation: pairing window open (2 min)");
        return;
    }
    if (role == IrrigationRole::GATEWAY) {
        if (ev == Ev::SHORT) {
            gatewayPairing.openWindow(millis());
            refreshLedMode();
            LOG_INFO("Irrigation: gateway accept window open (2 min)");
        } else if (ev == Ev::LONG_3S) {
            logFarmKey(); // export PSK to service device vault (spec §11.2)
        }
        return;
    }
    if (role == IrrigationRole::SERVICO) {
        // Fase 8b: device SERVICO. LONG = despeja o cofre no serial (bancada §11.7);
        // SHORT = sobe o portal (abas Clientes/Rede/Log chegam na 8c).
        if (ev == Ev::LONG_3S)
            svcExportToConsole();
        else if (ev == Ev::SHORT)
            portal.requestOpen(millis());
        return;
    }
    if (role == IrrigationRole::REPETIDOR) {
        // Repetidor provisionado: SHORT reabre a janela de acesso (spec 2026-08-11).
        if (ev == Ev::SHORT)
            applyAccessWindowShort();
        return;
    }
    if (role != IrrigationRole::ESTACAO)
        return;
    // Paired station gestures.
    switch (ev) {
    case Ev::SHORT:
        applyAccessWindowShort(); // Janela de acesso: REOPEN_LIVE ou REBOOT_TO_REOPEN (spec 2026-08-11)
        break;
    case Ev::DOUBLE:
        if (valves.isOpen(0)) {
            valves.close(0);
            sendEvento(EV_MANUAL_CLOSE);
            // §8.9: fechamento manual por botão físico.
            auditEvent(AuditOrigin::BOTAO_FISICO, AuditAction::FECHAR, 0, AuditResult::OK);
        } else if (valves.open(0, DEFAULT_MANUAL_OPEN_S, settings.maxOpenConfigS, millis()) ==
                   ValveController::Result::OK) {
            sendEvento(EV_MANUAL_OPEN, DEFAULT_MANUAL_OPEN_S);
            // §8.9: abertura manual por botão físico.
            auditEvent(AuditOrigin::BOTAO_FISICO, AuditAction::ABRIR, 0, AuditResult::OK);
        } else {
            // §8.9: abertura manual rejeitada (bateria baixa ou configuração).
            auditEvent(AuditOrigin::BOTAO_FISICO, AuditAction::ABRIR, 0, AuditResult::NACK);
        }
        break;
    case Ev::LONG_3S:
        if (valves.open(0, 10, settings.maxOpenConfigS, millis()) == ValveController::Result::OK) {
            sendEvento(EV_TEST_PULSE);
            // §8.9: pulso de teste por botão físico.
            auditEvent(AuditOrigin::BOTAO_FISICO, AuditAction::PULSO, 0, AuditResult::OK);
        } else {
            auditEvent(AuditOrigin::BOTAO_FISICO, AuditAction::PULSO, 0, AuditResult::NACK);
        }
        break;
    default:
        break;
    }
}

// Decision §5: LED priority table (spec §8.7).
// PAIRING > BATTERY_SOS > OUTPUT_OPEN > CONFIG_PENDING > NO_GATEWAY > NORMAL.
// NO_GATEWAY heuristic: bound but never received a packet from the gateway since boot.
// Refined in Phase 4 with a proper heartbeat timeout.
void IrrigationModule::refreshLedMode()
{
    using Mode = LedPatternController::Mode;
    uint16_t vbat = batteryCentiV();
    if (stationPairing.state() == StationPairing::State::WINDOW || gatewayPairing.windowOpen())
        led.setMode(Mode::PAIRING);
    else if (vbat != 0 && vbat < settings.vbatMinAbrirCentiV)
        led.setMode(Mode::BATTERY_SOS);
    else if (valves.stateBitmap() != 0)
        led.setMode(Mode::OUTPUT_OPEN);
    else if (safeMode)
        led.setMode(Mode::CONFIG_PENDING);
    else if (settings.boundGateway != 0 && lastGatewayRxMs == 0)
        led.setMode(Mode::NO_GATEWAY);
    else
        led.setMode(Mode::NORMAL);
}

// ---------------------------------------------------------------------------
// Allowlist persistence — same staged-write pattern as IrrigationSettings.
// formato = magic(4)+versão(1)+count(1)+ids LE (host little-endian).
// Buffer: MAGIC(4) + versão(1) + count(1) + ids[MAX](4*16) = 70 bytes max.
// ---------------------------------------------------------------------------

bool IrrigationModule::loadAllowlist()
{
#ifdef FSCom
    auto f = FSCom.open(ALLOWLIST_PATH, FILE_O_READ);
    if (!f)
        return false;
    uint8_t buf[6 + Allowlist::MAX * 4];
    size_t n = f.read(buf, sizeof(buf));
    f.close();
    return allowlist.deserialize(buf, n);
#else
    return false;
#endif
}

bool IrrigationModule::saveAllowlist()
{
#ifdef FSCom
    uint8_t buf[6 + Allowlist::MAX * 4];
    size_t n = allowlist.serialize(buf, sizeof(buf));
    if (n == 0)
        return false;
    // Staged write: write to .tmp then rename, so a power failure mid-write cannot
    // corrupt the active file (same pattern as saveIrrigationSettings).
    auto f = FSCom.open(ALLOWLIST_TMP, FILE_O_WRITE);
    if (!f)
        return false;
    size_t w = f.write(buf, n);
    f.close();
    if (w != n) {
        FSCom.remove(ALLOWLIST_TMP);
        return false;
    }
    FSCom.remove(ALLOWLIST_PATH);
    if (!renameFile(ALLOWLIST_TMP, ALLOWLIST_PATH)) {
        LOG_ERROR("Irrigation allowlist rename failed");
        FSCom.remove(ALLOWLIST_TMP);
        return false;
    }
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Mini-log de auditoria (§8.9) — staged-write, espelha padrão da allowlist.
// Buffer: magic(4) + count(2) + reservado(2) + 100×16 bytes + CRC32(4) = 1612 bytes.
// ---------------------------------------------------------------------------

void IrrigationModule::auditEvent(AuditOrigin o, AuditAction a, uint8_t target, AuditResult res, uint32_t node, uint32_t seq)
{
    AuditRecord r = {};
    computeLocalSecs(r.tsSecs); // 0 se sem RTC no momento (tolerado pela spec)
    r.origin = (uint8_t)o;
    r.action = (uint8_t)a;
    r.target = target;
    r.result = (uint8_t)res;
    r.node = node;
    r.seq = seq;
    audit.append(r);
    auditDirty = true;
    // Fase 6b Task 16: persiste no log de flash do gateway (meses de histórico).
    // O FlashAuditRing grava header a cada append — não passa pelo flush de 60s.
    if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
        auditFlash.append(r);
}

bool IrrigationModule::loadAuditLog()
{
#ifdef FSCom
    auto f = FSCom.open(AUDIT_LOG_PATH, FILE_O_READ);
    if (!f)
        return false;
    uint8_t raw[8 + AUDIT_CAP * 16 + 4];
    size_t n = f.read(raw, sizeof(raw));
    f.close();
    return audit.deserialize(raw, n);
#else
    return false;
#endif
}

bool IrrigationModule::saveAuditLog()
{
#ifdef FSCom
    uint8_t raw[8 + AUDIT_CAP * 16 + 4];
    size_t n = audit.serialize(raw, sizeof(raw));
    if (n == 0)
        return false;
    // Staged write: grava em .tmp depois renomeia — falha de energia não corrompe o arquivo ativo.
    auto f = FSCom.open(AUDIT_LOG_TMP, FILE_O_WRITE);
    if (!f)
        return false;
    size_t w = f.write(raw, n);
    f.close();
    if (w != n) {
        FSCom.remove(AUDIT_LOG_TMP);
        return false;
    }
    FSCom.remove(AUDIT_LOG_PATH);
    if (!renameFile(AUDIT_LOG_TMP, AUDIT_LOG_PATH)) {
        LOG_ERROR("Irrigation auditlog rename failed");
        FSCom.remove(AUDIT_LOG_TMP);
        return false;
    }
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Gateway state persistence (Task 6, decisão §5) — staged-write, 4 arquivos.
// Helper macro para não repetir o padrão 4×.
// ---------------------------------------------------------------------------

// Staged-write genérico: serializa com fn, grava em tmp, rename.
// Tamanho máximo dos buffers:
//   stations: MAGIC(4)+ver(1)+count(1)+16*87 = 1398 bytes → 1400
//   zones:    MAGIC(4)+ver(1)+count(1)+24*29 = 702 bytes (buffer = 6 + MAX*29)
//   programs: MAGIC(4)+ver(1)+count(1)+8*... = ~600 bytes → 700
//   mirror:   MAGIC(4)+ver(1)+1             = 6    bytes → 16

static bool stagedWrite(const char *tmp, const char *path, const uint8_t *buf, size_t n)
{
#ifdef FSCom
    if (n == 0)
        return false;
    auto f = FSCom.open(tmp, FILE_O_WRITE);
    if (!f)
        return false;
    size_t w = f.write(buf, n);
    f.close();
    if (w != n) {
        FSCom.remove(tmp);
        return false;
    }
    FSCom.remove(path);
    if (!renameFile(tmp, path)) {
        LOG_ERROR("Irrigation rename failed: %s", path);
        FSCom.remove(tmp);
        return false;
    }
    return true;
#else
    (void)tmp; (void)path; (void)buf; (void)n;
    return false;
#endif
}

static bool stagedRead(const char *path, uint8_t *buf, size_t cap, size_t &outN)
{
#ifdef FSCom
    auto f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return false;
    outN = f.read(buf, cap);
    f.close();
    return outN > 0;
#else
    (void)path; (void)buf; (void)cap;
    outN = 0;
    return false;
#endif
}

bool IrrigationModule::loadGatewayState()
{
    size_t n = 0;
    bool ok = true;

    // Stations
    {
        uint8_t buf[StationRegistry::SERIALIZED_MAX];
        if (stagedRead(GW_STATIONS_PATH, buf, sizeof(buf), n))
            ok &= gateway.stations.deserialize(buf, n);
    }
    // Zones
    {
        uint8_t buf[6 + ZoneTable::MAX * 29];
        if (stagedRead(GW_ZONES_PATH, buf, sizeof(buf), n))
            ok &= gateway.zones.deserialize(buf, n);
    }
    // Programs
    {
        uint8_t buf[700];
        if (stagedRead(GW_PROGRAMS_PATH, buf, sizeof(buf), n))
            ok &= gateway.scheduler.deserialize(buf, n);
    }
    // Mirror (only flag)
    {
        uint8_t buf[16];
        if (stagedRead(GW_MIRROR_PATH, buf, sizeof(buf), n))
            ok &= gateway.mirror.deserialize(buf, n);
    }
    return ok;
}

bool IrrigationModule::saveGatewayState()
{
    bool ok = true;
    // Stations
    {
        uint8_t buf[StationRegistry::SERIALIZED_MAX];
        size_t n = gateway.stations.serialize(buf, sizeof(buf));
        ok &= stagedWrite(GW_STATIONS_TMP, GW_STATIONS_PATH, buf, n);
    }
    // Zones
    {
        uint8_t buf[6 + ZoneTable::MAX * 29];
        size_t n = gateway.zones.serialize(buf, sizeof(buf));
        ok &= stagedWrite(GW_ZONES_TMP, GW_ZONES_PATH, buf, n);
    }
    // Programs
    {
        uint8_t buf[700];
        size_t n = gateway.scheduler.serialize(buf, sizeof(buf));
        ok &= stagedWrite(GW_PROGRAMS_TMP, GW_PROGRAMS_PATH, buf, n);
    }
    // Mirror
    {
        uint8_t buf[16];
        size_t n = gateway.mirror.serialize(buf, sizeof(buf));
        ok &= stagedWrite(GW_MIRROR_TMP, GW_MIRROR_PATH, buf, n);
    }
    ok &= saveRemoteButtons(); // Modo Remoto: persiste associações botoeira→saída
    return ok;
}

// ---------------------------------------------------------------------------
// Fase 6b: persistência da tabela de intertravamentos (arquivo separado).
// ---------------------------------------------------------------------------

bool IrrigationModule::loadInterlocks()
{
    size_t n = 0;
    uint8_t buf[6 + InterlockTable::MAX * 48]; // margem folgada para serialização futura
    if (!stagedRead(GW_INTERLOCKS_PATH, buf, sizeof(buf), n))
        return false; // arquivo ausente na primeira inicialização — ok, tabela vazia
    return gateway.interlocks.deserialize(buf, n);
}

bool IrrigationModule::saveInterlocks()
{
    uint8_t buf[6 + InterlockTable::MAX * 48];
    size_t n = gateway.interlocks.serialize(buf, sizeof(buf));
    return stagedWrite(GW_INTERLOCKS_TMP, GW_INTERLOCKS_PATH, buf, n);
}

// ---------------------------------------------------------------------------
// Fase 6b Task 18: persistência de nomes de sensores (arquivo separado).
// Espelha loadInterlocks/saveInterlocks EXATAMENTE.
// ---------------------------------------------------------------------------

bool IrrigationModule::loadSensorNames()
{
    size_t n = 0;
    uint8_t buf[4 + 2 + SensorNameTable::MAX * sizeof(SensorName) + 4]; // magic+count+MAX×entry+crc
    if (!stagedRead(GW_SENSORNAMES_PATH, buf, sizeof(buf), n))
        return false; // arquivo ausente na primeira inicialização — ok, tabela vazia
    return gateway.sensorNames.deserialize(buf, n);
}

bool IrrigationModule::saveSensorNames()
{
    uint8_t buf[4 + 2 + SensorNameTable::MAX * sizeof(SensorName) + 4];
    size_t n = gateway.sensorNames.serialize(buf, sizeof(buf));
    return stagedWrite(GW_SENSORNAMES_TMP, GW_SENSORNAMES_PATH, buf, n);
}

// ---------------------------------------------------------------------------
// Fase 7a: persistência da tabela de grupos hidráulicos (arquivo separado).
// Espelha loadInterlocks/saveInterlocks EXATAMENTE.
// ---------------------------------------------------------------------------

bool IrrigationModule::loadGroups()
{
    size_t n = 0;
    uint8_t buf[6 + HydraulicGroupTable::MAX * 41 + 4]; // margem folgada (entry real 39 B)
    if (!stagedRead(GW_GRUPOS_PATH, buf, sizeof(buf), n))
        return false; // ausente na 1ª init — ok, tabela vazia
    return gateway.groups.deserialize(buf, n);
}

bool IrrigationModule::saveGroups()
{
    uint8_t buf[6 + HydraulicGroupTable::MAX * 41 + 4];
    size_t n = gateway.groups.serialize(buf, sizeof(buf));
    return stagedWrite(GW_GRUPOS_TMP, GW_GRUPOS_PATH, buf, n);
}

// Persistência da tabela de controle de nível (arquivo separado). Espelha loadGroups/saveGroups.
bool IrrigationModule::loadLevels()
{
    size_t n = 0;
    uint8_t buf[6 + LevelControlTable::MAX * sizeof(LevelRule) + 4];
    if (!stagedRead(GW_NIVEIS_PATH, buf, sizeof(buf), n))
        return false; // ausente na 1ª init — ok, tabela vazia
    return gateway.levels.deserialize(buf, n);
}

bool IrrigationModule::saveLevels()
{
    uint8_t buf[6 + LevelControlTable::MAX * sizeof(LevelRule) + 4];
    size_t n = gateway.levels.serialize(buf, sizeof(buf));
    return stagedWrite(GW_NIVEIS_TMP, GW_NIVEIS_PATH, buf, n);
}

// Persistência de clima (config + regras). Espelha loadLevels/saveLevels.
bool IrrigationModule::loadWeather()
{
    bool ok = true;
    size_t n = 0;
    {
        uint8_t buf[WeatherConfig::SERIALIZED];
        if (stagedRead(GW_WEATHERCFG_PATH, buf, sizeof(buf), n))
            ok &= gateway.weatherConfig.deserialize(buf, n);
    }
    {
        uint8_t buf[4 + 2 + WeatherRuleTable::MAX * sizeof(WeatherRule) + 4];
        if (stagedRead(GW_WEATHERRULES_PATH, buf, sizeof(buf), n))
            ok &= gateway.weatherRules.deserialize(buf, n);
    }
    return ok;
}

bool IrrigationModule::saveWeatherConfig()
{
    uint8_t buf[WeatherConfig::SERIALIZED];
    size_t n = gateway.weatherConfig.serialize(buf, sizeof(buf));
    return stagedWrite(GW_WEATHERCFG_TMP, GW_WEATHERCFG_PATH, buf, n);
}

bool IrrigationModule::saveWeatherRules()
{
    uint8_t buf[4 + 2 + WeatherRuleTable::MAX * sizeof(WeatherRule) + 4];
    size_t n = gateway.weatherRules.serialize(buf, sizeof(buf));
    return stagedWrite(GW_WEATHERRULES_TMP, GW_WEATHERRULES_PATH, buf, n);
}

// Gate de supressão climática: retorna veredito para a zona (OR zona-direta + grupo-dono).
// Fail-open: sem config habilitada, sem RTC ou cache velho → WeatherVerdict{} (suppress=false).
WeatherVerdict IrrigationModule::weatherVerdictForZone(uint8_t zoneId)
{
    if (!gateway.weatherConfig.enabled)
        return WeatherVerdict{};
    uint32_t nowLocal = 0;
    computeLocalSecs(nowLocal);
    const WeatherCache &cache = gateway.weatherCache;
    const WeatherRuleTable &rules = gateway.weatherRules;
    const uint16_t ttl = gateway.weatherConfig.staleTtlH;
    // 1) veredito direto por zona
    WeatherVerdict v = WeatherEngine::zoneVerdict(zoneId, cache, rules, nowLocal, ttl);
    if (v.suppress)
        return v;
    // 2) veredito pelo grupo-dono (se a zona for membro de algum grupo hidráulico).
    //    Usa gateway.groups.byZone() — mesmo acesso que routeZoneToGroup.
    const HydraulicGroup *hg = gateway.groups.byZone(zoneId);
    if (hg) {
        WeatherVerdict gv = WeatherEngine::groupVerdict(hg->id, cache, rules, nowLocal, ttl);
        if (gv.suppress)
            return gv;
    }
    return WeatherVerdict{};
}

// ---------------------------------------------------------------------------
// Task 5: controle de nível por boia — executor de zona, tick, apply-helpers.
// ---------------------------------------------------------------------------

// Abre/fecha uma zona pelo caminho canônico: grupo hidráulico se for zona-membro,
// senão comando direto de válvula/GPO. Espelha o bloco OPEN/CLOSE do scheduler.
void IrrigationModule::gwDriveZone(uint8_t zoneId, bool open, uint16_t durS)
{
    const Zone *z = gateway.zones.byId(zoneId);
    if (!z)
        return;
    const StationEntry *st = gateway.stations.byNode(z->node);
    uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
    if (open) {
        if (routeZoneToGroup(zoneId, true, durS)) // grupo cuida da coreografia da bomba
            return;
        // Zona livre (bomba fora de grupo): o caminho direto NÃO passa por routeZoneToGroup,
        // então repete aqui o veto de intertravamento que todo open canônico aplica — senão o
        // controle de nível reabriria uma bomba fechada+bloqueada por segurança (review I1).
        if (gateway.interlockEngine.zoneVerdict(zoneId).bloqueada) {
            auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::CMD_REJEITADO, zoneId, AuditResult::NACK, z->node);
            return;
        }
        gwSendValveCmd(z->node, z->index, z->tipo, 1, durS, z->id, attempts);
    } else {
        if (routeZoneToGroup(zoneId, false, 0))
            return;
        gateway.openGate.release(zoneId);
        gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
    }
}

// Avalia as regras de controle de nível 1×/tick. Resolve cada boia no cache de
// telemetria (leitura digital + frescor via atMs) e executa os intents do engine.
void IrrigationModule::gwLevelTick(uint32_t nowMs)
{
    if (gateway.levels.count() == 0)
        return;

    LevelInput inputs[LevelControlTable::MAX];
    for (size_t i = 0; i < gateway.levels.count() && i < LevelControlTable::MAX; i++) {
        const LevelRule *r = gateway.levels.ruleAt(i);
        LevelInput in{};
        const StationTelemetry *t = gateway.telemetry.byNode(r->sensorNode);
        if (t && t->node) {
            for (uint8_t k = 0; k < t->sensorCount && k < IrrigationProto::HB_MAX_SENSORS; k++)
                if (t->sensors[k].id == r->sensorIdx) {
                    in.present = true;
                    in.active = (t->sensors[k].valueCenti != 0);
                    break;
                }
            in.fresh = ((uint32_t)(nowMs - t->atMs) <= (uint32_t)r->staleTimeoutS * 1000u);
        }
        inputs[i] = in;
    }

    LevelIntent out[LevelControlTable::MAX];
    size_t n = gateway.levelEngine.evaluate(gateway.levels, inputs, gateway.levels.count(), nowMs, out,
                                            LevelControlTable::MAX);
    for (size_t i = 0; i < n; i++) {
        const LevelIntent &it = out[i];
        switch (it.act) {
        case LevelIntent::Act::START:
        case LevelIntent::Act::RENEW:
            gwDriveZone(it.zoneId, true, it.durS);
            if (it.act == LevelIntent::Act::START) {
                const Zone *z = gateway.zones.byId(it.zoneId);
                auditEvent(AuditOrigin::NIVEL, AuditAction::ABRIR, it.zoneId, AuditResult::OK, z ? z->node : 0);
            }
            break;
        case LevelIntent::Act::STOP:
            gwDriveZone(it.zoneId, false, 0);
            {
                const Zone *z = gateway.zones.byId(it.zoneId);
                auditEvent(AuditOrigin::NIVEL, AuditAction::FECHAR, it.zoneId, AuditResult::OK, z ? z->node : 0);
            }
            break;
        case LevelIntent::Act::STALE_STOP: {
            gwDriveZone(it.zoneId, false, 0);
            const Zone *z = gateway.zones.byId(it.zoneId);
            auditEvent(AuditOrigin::NIVEL, AuditAction::FECHAR, it.zoneId, AuditResult::TIMEOUT, z ? z->node : 0);
            const LevelRule *r = gateway.levels.byId(it.ruleId);
            Alert a{};
            a.type = AlertType::NIVEL_BOIA_MUDA;
            a.node = r ? r->sensorNode : 0;
            a.arg = it.zoneId;
            a.atMs = nowMs;
            gateway.alerts.push(a);
            break;
        }
        case LevelIntent::Act::NONE:
            break;
        }
    }
}

bool IrrigationModule::gwApplyLevelUpsert(LevelRule &r, char *err, size_t errCap)
{
    if (r.id == 0) { // aloca menor id livre 1..MAX
        for (uint8_t cand = 1; cand <= LevelControlTable::MAX; cand++)
            if (!gateway.levels.byId(cand)) {
                r.id = cand;
                break;
            }
        if (r.id == 0) {
            snprintf(err, errCap, "tabela de niveis cheia");
            return false;
        }
    }
    if (!gateway.levels.upsert(r)) {
        snprintf(err, errCap, "tabela de niveis cheia");
        return false;
    }
    saveLevels();
    return true;
}

bool IrrigationModule::gwApplyLevelDelete(uint8_t id)
{
    if (!gateway.levels.removeById(id))
        return false;
    saveLevels();
    return true;
}

// ---------------------------------------------------------------------------
// Fase 6b, Task 14b: reconstrói e empurra regras locais de intertravamento
// para cada estação conhecida. Deve ser chamado no init (após loadInterlocks +
// loadGatewayState) e após mutações na tabela de interlocks (Task 18).
//
// Epoch idiom: o gateway nunca "inventa" um epoch novo; ele usa
//   entry->desiredEpoch + 1
// para sinalizar que o blob foi modificado pelo gateway, superando o epoch
// que a estação já conhece. O guard de memcmp garante que reboots sem
// mudança real não causam epoch-bump nem churn de push.
// ---------------------------------------------------------------------------
void IrrigationModule::gwRebuildLocalInterlocks()
{
    bool anyChanged = false;

    for (size_t si = 0; si < gateway.stations.count(); si++) {
        // Acessa a entrada mutável (por índice de ocupação).
        // nodeAt() retorna const; precisamos do ponteiro mutável via mutableByNode.
        const StationEntry *centry = gateway.stations.nodeAt(si);
        if (!centry || centry->node == 0)
            continue;
        StationEntry *entry = gateway.stations.mutableByNode(centry->node);
        if (!entry)
            continue;

        // Só faz sentido modificar entradas que já receberam blob válido (desiredEpoch != 0).
        // Se desiredEpoch == 0 a entrada ainda não tem blob e não deve ser tocada (§5.4).
        if (entry->desiredEpoch == 0)
            continue;

        // 1. Carrega a config desejada atual do blob.
        IrrigationSettings s;
        if (!migrateIrrigationSettings(entry->blob, sizeof(entry->blob), s))
            continue;

        // 2. Guarda o estado anterior dos slots locais para comparação final.
        IrrigationSettings::LocalInterlock oldRules[IrrigationSettings::MAX_LOCAL_INTERLOCKS];
        memcpy(oldRules, s.localInterlocks, sizeof(oldRules));

        // 3. Zera os slots (serão recomputados a seguir).
        for (auto &li : s.localInterlocks)
            li = {};

        // 4. Percorre todas as regras de intertravamento do gateway e calcula
        //    as máscaras de saída locais para esta estação.
        uint8_t nextSlot = 0;
        for (size_t ri = 0; ri < InterlockTable::MAX && nextSlot < IrrigationSettings::MAX_LOCAL_INTERLOCKS; ri++) {
            const InterlockRule *r = gateway.interlocks.ruleAtSlot(ri);
            if (!r || r->id == 0)
                continue; // slot vazio
            if (r->tipo != IL_SENSOR)
                continue; // só regras de sensor geram réplica local
            if (r->node != entry->node)
                continue; // regra pertence a outra estação

            // Calcula quais saídas desta estação são cobertas pela regra.
            uint8_t valvMask = 0, gpoMask = 0;
            for (size_t zi = 0; zi < gateway.zones.count(); zi++) {
                const Zone *z = gateway.zones.zoneAt(zi);
                if (!z || z->node != entry->node)
                    continue;
                bool coberta = r->todas;
                if (!coberta) {
                    for (uint8_t k = 0; k < 8; k++) {
                        if (r->zoneIds[k] == 0)
                            break;
                        if (r->zoneIds[k] == z->id) {
                            coberta = true;
                            break;
                        }
                    }
                }
                if (!coberta)
                    continue;
                if (z->tipo == 1)
                    gpoMask |= (uint8_t)(1u << (z->index & 7));
                else
                    valvMask |= (uint8_t)(1u << (z->index & 7));
            }

            // Se a regra não afeta nenhuma saída local desta estação, pula.
            if (!valvMask && !gpoMask)
                continue;

            // Preenche o próximo slot livre.
            IrrigationSettings::LocalInterlock &li = s.localInterlocks[nextSlot++];
            li.sensorIdx       = r->sensorIdx;
            li.condicao        = r->condicao;
            li.acao            = r->acao;
            li.saidasValvMask  = valvMask;
            li.valorCenti      = r->valorCenti;
            li.histereseCenti  = r->histereseCenti;
            li.saidasGpoMask   = gpoMask;
            li.pad             = 0;
        }

        // 5. Verifica se algo mudou (guard contra boot-churn).
        if (memcmp(oldRules, s.localInterlocks, sizeof(oldRules)) == 0)
            continue; // sem mudança: não toca o epoch nem o blob

        // 6. Escreve o blob de volta (version e magic vêm intactos do migrate→v5).
        //    Epoch sobe em +1 para sinalizar ao mecanismo de reconciliação que o
        //    gateway quer empurrar esta config atualizada para a estação.
        memcpy(entry->blob, &s, sizeof(s));
        entry->desiredEpoch += 1;
        anyChanged = true;
        LOG_INFO("Irrigation GW: rebuilt local interlocks for node=0x%08x (%u slots), epoch→%u",
                 entry->node, nextSlot, entry->desiredEpoch);
    }

    // 7. Persiste o estado do gateway uma única vez se qualquer estação mudou.
    if (anyChanged)
        saveGatewayState();
}

// ---------------------------------------------------------------------------
// Gateway engine — handlers e tick (Task 6, decisões 1–6)
// ---------------------------------------------------------------------------

// Decisão §1: envia CmdValvula ou CmdGpo e registra no tracker.
uint32_t IrrigationModule::gwSendValveCmd(uint32_t node, uint8_t index, uint8_t tipo, uint8_t action, uint16_t durationS,
                                          uint8_t zoneId, uint8_t attempts, AuditOrigin origin)
{
    // Alvo = próprio gateway ⇒ aciona a saída LOCAL diretamente (sem rádio).
    if (isLocalTarget(node, nodeDB->getNodeNum())) {
        // Modo seguro: bloqueia ativação (paridade com a estação); desligar segue permitido.
        if (safeMode && action == 1) {
            auditEvent(origin, tipo == 1 ? AuditAction::GPO_ON : AuditAction::ABRIR, index, AuditResult::NACK, node);
            return 0;
        }
        if (tipo == 1) {
            gpos.command(index, action, durationS, millis());
        } else if (action) {
            valves.open(index, durationS, 0 /*teto compilado; clamp de maxMin já no chamador*/, millis());
        } else {
            valves.close(index);
        }
        auditEvent(origin,
                   action ? (tipo == 1 ? AuditAction::GPO_ON : AuditAction::ABRIR)
                          : (tipo == 1 ? AuditAction::GPO_OFF : AuditAction::FECHAR),
                   index, AuditResult::OK, node);
        uint32_t usedSeq = ++txSeq;
        // Enfileira ACK sintético (drenado no gwTick, após noteSent do motor de grupos).
        if (pendingLocalAckCount < 8) {
            pendingLocalAck[pendingLocalAckCount].node = node;
            pendingLocalAck[pendingLocalAckCount].seq = usedSeq;
            pendingLocalAckCount++;
        }
        LOG_DEBUG("Irrigation GW: drive LOCAL zone=%u idx=%u tipo=%u action=%u dur=%u", zoneId, index, tipo, action,
                  durationS);
        return usedSeq;
    }

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = node;
    uint32_t usedSeq = ++txSeq; // mesmo seq p/ CmdGpo/CmdValvula e o tracker.track abaixo
    if (tipo == 1) {
        CmdGpo cmd = {};
        cmd.gpoId = index;
        cmd.action = action;
        cmd.durationS = durationS;
        p->decoded.payload.size = (uint16_t)encodeCmdGpo(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), usedSeq, cmd);
    } else {
        CmdValvula cmd = {};
        cmd.valveId = index;
        cmd.action = action;
        cmd.durationS = durationS;
        p->decoded.payload.size = (uint16_t)encodeCmdValvula(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), usedSeq, cmd);
    }
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return 0;
    }
    txPacket(p);
    gateway.tracker.track(usedSeq, node, zoneId, action, durationS, attempts, millis());
    LOG_DEBUG("Irrigation GW: sent cmd zone=%u node=0x%08x action=%u dur=%u", zoneId, node, action, durationS);
    return usedSeq;
}

// Fase 6b Task 15: envia MSG_CMD_MAINT a uma estação para abrir/fechar janela de manutenção do tamper.
// minutes == 0 fecha a janela imediatamente. Task 18 chamará este helper do endpoint.
void IrrigationModule::gwSendMaintWindow(uint32_t node, uint16_t minutes)
{
    CmdMaint cmd = {};
    cmd.durationMin = minutes;
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = node;
    p->decoded.payload.size =
        (uint16_t)encodeCmdMaint(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, cmd);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    txPacket(p);
    LOG_INFO("Irrigation GW: CMD_MAINT enviado para 0x%08x (%u min)", node, minutes);
}

// Fonte única de hora local do gateway (mesma que o scheduler consome no gwTick).
bool IrrigationModule::computeLocalSecs(uint32_t &out) const
{
    uint32_t epochLocal = getValidTime(RTCQualityDevice, true);
    out = epochLocal;
    return epochLocal != 0;
}

// --- Fase 8b: cola da página Horário (fonte de hora / manual / fuso / sync NTP) ---
size_t IrrigationModule::gwBuildTimeStatus(char *buf, size_t cap)
{
    IrrigationWeb::TimeStatusCtx c = {};
    // Epoch UTC real (local=false): o navegador aplica o fuso ao formatar. Passar local=true
    // embutiria o offset do fuso e o cliente o somaria de novo (hora dobrada).
    c.nowEpoch = getValidTime(RTCQualityDevice, false);
    c.quality = (int)getRTCQuality();
#if defined(ARCH_ESP32)
    c.staUp = WiFi.isConnected();
    unsigned long last = ntpLastRunMs();
    c.lastSyncS = (last != 0) ? (int32_t)((millis() - last) / 1000UL) : -1;
#else
    c.staUp = false;
    c.lastSyncS = -1;
#endif
    c.ntpServer = config.network.ntp_server[0] ? config.network.ntp_server : "pool.ntp.org";
    c.tz = config.device.tzdef; // "" se não definido
    return IrrigationWeb::buildTimeStatus(c, buf, cap);
}

bool IrrigationModule::gwSetManualTime(uint32_t epoch)
{
    if (epoch < 1600000000u)
        return false;
    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    perhapsSetRTC(RTCQualityDevice, &tv, /*forceUpdate=*/true);
    LOG_INFO("Irrigation GW: hora definida manualmente (epoch=%u)", epoch);
    return true;
}

bool IrrigationModule::gwSetTimezone(const char *posix)
{
    if (!posix || !IrrigationWeb::tzIsValidPreset(posix))
        return false;
    strncpy(config.device.tzdef, posix, sizeof(config.device.tzdef) - 1);
    config.device.tzdef[sizeof(config.device.tzdef) - 1] = '\0';
    setenv("TZ", config.device.tzdef, 1);
    tzset(); // aplica o fuso já neste boot (localtime passa a usar o novo TZ)
    // Persiste só o segmento de config (main.cpp relê tzdef no próximo boot). Evita o
    // reloadConfig(), que dispara reconfig de rádio/observers — desnecessário p/ um fuso.
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    LOG_INFO("Irrigation GW: fuso ajustado (%s)", config.device.tzdef);
    return true;
}

bool IrrigationModule::gwSyncNtpNow()
{
#if defined(ARCH_ESP32)
    if (!WiFi.isConnected())
        return false;
    triggerNtpUpdate();
    return true;
#else
    return false;
#endif
}

// --- Supressão meteorológica (Task 10) ---
bool IrrigationModule::gwWeatherSetConfig(uint8_t enabled, int32_t latE7, int32_t lonE7)
{
    gateway.weatherConfig.enabled = enabled ? 1 : 0;
    gateway.weatherConfig.latE7 = latE7;
    gateway.weatherConfig.lonE7 = lonE7;
    return saveWeatherConfig();
}

uint8_t IrrigationModule::gwWeatherUpsertRule(const WeatherRule &rIn)
{
    WeatherRule r = rIn;
    if (!r.id) {
        r.id = gateway.weatherRules.nextFreeId();
        if (!r.id)
            return 0; // tabela cheia
    }
    if (!gateway.weatherRules.upsert(r))
        return 0;
    if (!saveWeatherRules())
        return 0;
    return r.id;
}

bool IrrigationModule::gwWeatherDeleteRule(uint8_t id)
{
    if (!gateway.weatherRules.removeById(id))
        return false;
    return saveWeatherRules();
}

bool IrrigationModule::gwWeatherRefresh()
{
#if defined(ARCH_ESP32)
    if (!WiFi.isConnected())
        return false;
    uint32_t nowLocal = 0;
    computeLocalSecs(nowLocal);
    return weatherClient.pollNow(gateway.weatherConfig, nowLocal, gateway.weatherCache);
#else
    return false;
#endif
}

// --- Serviço do painel web (gateway). Ponte entre a cola HTTP (Task 10) e o estado do gateway. ---
bool IrrigationModule::gwIsGateway() const
{
    return settings.role == (uint8_t)IrrigationRole::GATEWAY;
}

// --- Fase 6b Task 18: gwApply* para intertravamentos/sensores (remove const_cast dos endpoints) ---
bool IrrigationModule::gwApplyInterlockUpsert(const InterlockRule &r)
{
    if (!gateway.interlocks.upsert(r))
        return false;
    saveInterlocks();
    gwRebuildLocalInterlocks();
    return true;
}
bool IrrigationModule::gwApplyInterlockDelete(uint8_t id)
{
    if (!gateway.interlocks.removeById(id))
        return false;
    saveInterlocks();
    gwRebuildLocalInterlocks();
    return true;
}
bool IrrigationModule::gwApplyGroupUpsert(HydraulicGroup &g, char *err, size_t errCap)
{
    if (!IrrigationWeb::validateGroupZones(g, gateway.zones, err, errCap))
        return false; // err preenchido
    if (g.id == 0) {
        // Aloca 1º id livre 1..MAX.
        uint8_t freeId = 0;
        for (uint8_t cand = 1; cand <= HydraulicGroupTable::MAX; cand++) {
            if (!gateway.groups.byId(cand)) { freeId = cand; break; }
        }
        if (freeId == 0) {
            snprintf(err, errCap, "tabela de grupos cheia");
            return false;
        }
        g.id = freeId;
    }
    if (!gateway.groups.upsert(g)) {
        snprintf(err, errCap, "grupo invalido (zona em outro grupo?)");
        return false;
    }
    saveGroups();
    return true;
}

bool IrrigationModule::gwApplyGroupDelete(uint8_t id)
{
    if (!gateway.groups.removeById(id))
        return false;
    saveGroups();
    return true;
}

bool IrrigationModule::gwRunGroupCommand(uint8_t id, bool open, uint16_t durationS)
{
    const HydraulicGroup *g = gateway.groups.byId(id);
    if (!g)
        return false;
    uint16_t dur = durationS;
    if (open && dur == 0) {
        // Default: maior maxMin (s) entre os membros; fallback 600 s.
        uint16_t best = 0;
        for (uint8_t i = 0; i < g->zoneCount && i < 8; i++) {
            const Zone *z = gateway.zones.byId(g->zoneIds[i]);
            if (z && z->maxMin > 0) {
                uint16_t candSecs = (uint16_t)(z->maxMin * 60);
                if (candSecs > best) best = candSecs;
            }
        }
        dur = best ? best : 600;
    }
    if (open && dur > HydraulicGroupEngine::PUMP_CEILING_S)
        dur = HydraulicGroupEngine::PUMP_CEILING_S;
    bool anyOpened = false;
    for (uint8_t i = 0; i < g->zoneCount && i < 8; i++) {
        uint8_t zid = g->zoneIds[i];
        if (open) {
            if (gateway.interlockEngine.zoneVerdict(zid).bloqueada)
                continue; // pula zona bloqueada
            gateway.groupEngine.setDesired(id, zid, true, dur);
            anyOpened = true;
        } else {
            gateway.groupEngine.setDesired(id, zid, false, 0);
        }
    }
    if (open && !anyOpened)
        auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, id, AuditResult::NACK);
    else
        auditEvent(AuditOrigin::PAINEL, open ? AuditAction::ABRIR : AuditAction::FECHAR, id, AuditResult::OK);
    return true;
}

bool IrrigationModule::gwApplySensorName(uint32_t node, uint8_t sensorIdx, const char *name)
{
    if (!gateway.sensorNames.set(node, sensorIdx, name))
        return false;
    saveSensorNames();
    return true;
}
void IrrigationModule::gwOpenMaintWindow(uint32_t node, uint16_t minutes)
{
    gwSendMaintWindow(node, minutes);
}

bool IrrigationModule::gwHasRtc() const
{
    uint32_t s = 0;
    return computeLocalSecs(s);
}
bool IrrigationModule::gwPairingPending() const
{
    return gatewayPairing.hasPending(millis());
}
uint32_t IrrigationModule::gwPairingNode() const
{
    return gatewayPairing.pendingNode();
}
uint16_t IrrigationModule::gwPairingSecondsLeft() const
{
    return gatewayPairing.pendingSecondsLeft(millis());
}
uint32_t IrrigationModule::gwLocalSecs() const
{
    uint32_t s = 0;
    computeLocalSecs(s);
    return s;
}

bool IrrigationModule::gwStaConnected() const
{
#if defined(ARCH_ESP32)
    return WiFi.isConnected();
#else
    return false;
#endif
}

const char *IrrigationModule::gwNodeLabel() const
{
    if (owner.long_name[0])
        return owner.long_name;
    return "Gateway";
}

uint32_t IrrigationModule::gwSelfNode() const
{
    return nodeDB->getNodeNum();
}

bool IrrigationModule::gwApplyZoneUpsert(const Zone &z)
{
    if (!gateway.zones.upsert(z))
        return false;
    saveGatewayState();
    return true;
}
bool IrrigationModule::gwApplyZoneDelete(uint8_t id)
{
    bool ok = gateway.zones.removeById(id);
    if (ok)
        saveGatewayState();
    return ok;
}
bool IrrigationModule::gwApplyProgramUpsert(const Program &p)
{
    if (!gateway.scheduler.upsert(p))
        return false;
    saveGatewayState();
    return true;
}
bool IrrigationModule::gwApplyProgramToggle(uint8_t id, bool enabled)
{
    // Localiza o programa atual e re-upsert com enabled ajustado.
    for (size_t i = 0; i < gateway.scheduler.count(); i++) {
        const Program *cur = gateway.scheduler.programAt(i);
        if (cur && cur->id == id) {
            Program np = *cur;
            np.enabled = enabled;
            gateway.scheduler.upsert(np);
            saveGatewayState();
            return true;
        }
    }
    return false;
}
bool IrrigationModule::gwApplyProgramDelete(uint8_t id)
{
    // Só mexe na saída se o programa EXCLUÍDO for o que está em execução. removeById()
    // já aborta o scheduler internamente, mas não emite o CLOSE de rádio — a cola fecha
    // a zona corrente aqui. Excluir um programa ocioso não deve perturbar outro em curso.
    if (gateway.scheduler.runningProgramId() == id && gateway.scheduler.currentZone() != 0) {
        const Zone *z = gateway.zones.byId(gateway.scheduler.currentZone());
        if (z)
            gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
    }
    bool ok = gateway.scheduler.removeById(id);
    if (ok)
        saveGatewayState();
    return ok;
}
bool IrrigationModule::gwRunCommand(const IrrigationWeb::WebCommand &c)
{
    using K = IrrigationWeb::CmdKind;
    if (c.kind == K::APPROVE_PAIRING) {
        // Aprovar pareamento pelo painel = mesmo gesto do botão SHORT no gateway:
        // abre a janela de aceite (2 min) para conceder aos anúncios que chegarem.
        // NÃO é commitPairing() (esse é o commit do lado ESTAÇÃO: grava PSK e reinicia).
        gatewayPairing.openWindow(millis());
        refreshLedMode();
        return true;
    }
    if (c.kind == K::ACK_ALERT) {
        if (c.atMs != 0)
            gateway.alerts.ackMatch(c.node, (AlertType)c.alertType, c.arg, c.atMs); // ack por-alerta (botão)
        else
            lastAckAllMs = millis(); // sem identidade ⇒ reconhecer todos (legado)
        return true;
    }
    const Zone *z = gateway.zones.byId(c.zoneId);
    if (!z)
        return false;
    const StationEntry *st = gateway.stations.byNode(z->node);
    uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
    if (c.kind == K::OPEN || c.kind == K::PULSE_TEST) {
        // Comando manual do painel: override explícito do operador — sem supressão de
        // espelho (diferente do gwTick, onde o scheduler cede a zona ao espelho).
        // Fase 6b: bloqueia abertura manual se intertravamento está ativo para esta zona.
        if (gateway.interlockEngine.zoneVerdict(z->id).bloqueada) {
            auditEvent(AuditOrigin::PAINEL, AuditAction::CMD_REJEITADO, z->id, AuditResult::NACK, z->node);
            return false;
        }
        uint16_t dur = c.durationS;
        if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60))
            dur = (uint16_t)(z->maxMin * 60);
        // Fase 7b: zona de grupo vai pelo motor (bomba + coreografia), não válvula direta.
        if (routeZoneToGroup(z->id, true, dur))
            return true;
        gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts);
        return true;
    }
    if (c.kind == K::CLOSE) {
        if (routeZoneToGroup(z->id, false, 0))
            return true;
        gateway.openGate.release(z->id); // balanceia a contagem de simultaneidade (no-op se não estava aberta)
        gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
        return true;
    }
    return false;
}

// --- Fase 9: painel de estação (config / remoção / pulso) ---

bool IrrigationModule::gwApplyStationConfig(const IrrigationWeb::StationConfigReq &r)
{
    StationEntry *e = gateway.stations.mutableByNode(r.node);
    if (!e || e->desiredEpoch == 0)
        return false; // sem blob adotado ainda (§5.4): nada a editar/empurrar
    IrrigationSettings cfg;
    if (!migrateIrrigationSettings(e->blob, sizeof(e->blob), cfg))
        return false;
    cfg.hbMinutes = r.hbMinutes;
    cfg.vbatAvisoCentiV = r.vbatAvisoCentiV;
    cfg.vbatCriticaCentiV = r.vbatCriticaCentiV;
    cfg.latE7 = r.latE7;
    cfg.lonE7 = r.lonE7;
    memcpy(e->blob, &cfg, sizeof(e->blob));
    e->desiredEpoch += 1;   // §5.4: bump → estação fica "pendente" até ACKar o novo epoch
    e->lat = r.latE7 / 100; // ×1e7 (blob) → ×1e5 (exibição do /stations)
    e->lon = r.lonE7 / 100;
    saveGatewayState();
    const StationTelemetry *tel = gateway.telemetry.byNode(r.node);
    gwReconcileEpoch(r.node, tel ? tel->configEpoch : 0); // push imediato do SET_CONFIG (respeita cooldown)
    return true;
}

int IrrigationModule::gwCountZonesForNode(uint32_t node) const
{
    int n = 0;
    for (size_t i = 0; i < gateway.zones.count(); i++) {
        const Zone *z = gateway.zones.zoneAt(i);
        if (z && z->node == node)
            n++;
    }
    return n;
}

bool IrrigationModule::gwRemoveStation(uint32_t node)
{
    if (gwCountZonesForNode(node) > 0)
        return false; // trava: mova as zonas vinculadas primeiro
    if (!gateway.stations.removeByNode(node))
        return false;
    // Limpa cooldown de epoch do nó (telemetria é só-RAM e expira sozinha).
    for (uint8_t i = 0; i < StationRegistry::MAX; i++)
        if (epochCooldowns[i].node == node) {
            epochCooldowns[i].node = 0;
            epochCooldowns[i].lastMs = 0;
        }
    saveGatewayState();
    return true;
}

bool IrrigationModule::gwStationPulse(const IrrigationWeb::StationPulseReq &r)
{
    const StationEntry *e = gateway.stations.byNode(r.node);
    if (!e)
        return false;
    // Valida que a saída existe no blob desejado (pino configurado != -1), quando há blob.
    IrrigationSettings cfg;
    if (e->desiredEpoch != 0 && migrateIrrigationSettings(e->blob, sizeof(e->blob), cfg)) {
        if (r.tipo == 1) {
            if (r.index >= IrrigationSettings::MAX_GPO || cfg.pinsGpo[r.index] < 0)
                return false;
        } else {
            if (r.index >= IrrigationSettings::MAX_VALVES || cfg.pinsHbridgeA[r.index] < 0)
                return false;
        }
    }
    uint8_t attempts = e->retries > 0 ? e->retries : 3;
    gwSendValveCmd(r.node, r.index, r.tipo, 1 /*abrir*/, r.durationS, 0 /*sem zona*/, attempts);
    return true;
}

// --- Serviço do portal de campo (Fase 5b). Role-agnóstico. ---
void IrrigationModule::portalFillNodeState(IrrigationWeb::NodeStateCtx &out) const
{
    out.role = settings.role;
    out.name = owner.short_name; // extern meshtastic_User& (mesmo uso de handlePairAnnounce)
    out.boundGateway = settings.boundGateway;
    out.configEpoch = settings.configEpoch;
    out.safeMode = safeMode;
    out.provisioned = provisioned;
    out.numValves = settings.numValves;
    out.numGpos = countGpos(settings);
    out.valveStates = valves.stateBitmap();
    out.gpoStates = gpos.states();
    out.vbatCentiV = batteryCentiV();
    out.vpanelCentiV = 0; // tensão de painel não medida na estação por enquanto (follow-up)
    out.flags = safeMode ? HB_FLAG_SAFE_MODE : 0;
    out.apSecondsLeft = portal.secondsLeft(millis());
    out.uptimeS = millis() / 1000;
    out.nowEpoch = getValidTime(RTCQualityDevice, false); // UTC real; o portal aplica o fuso ao formatar
    out.hasTime = out.nowEpoch != 0;
}

void IrrigationModule::noteGatewayLink(int8_t snrQ, int16_t rssi)
{
    linkSnrQ = snrQ;
    linkRssi = rssi;
    // normaliza SNR (~ -10..+10 dB → 0..80 quarter-dB deslocado) p/ 0..100 (altura de barra)
    int v = snrQ + 40;
    if (v < 0) v = 0;
    if (v > 80) v = 80;
    linkHist[linkHistHead] = (uint8_t)(v * 100 / 80);
    linkHistHead = (linkHistHead + 1) % 12;
    if (linkHistCount < 12)
        linkHistCount++;
}

void IrrigationModule::portalFillLink(IrrigationWeb::LinkCtx &out) const
{
    out.snrQuarterDb = linkSnrQ;
    out.rssiDbm = linkRssi;
    out.histCount = linkHistCount;
    for (uint8_t i = 0; i < linkHistCount; i++)
        out.hist[i] = linkHist[(linkHistHead + 12 - linkHistCount + i) % 12];
    out.neighborCount = 0;
    size_t total = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < total && out.neighborCount < 8; i++) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == nodeDB->getNodeNum())
            continue;
        IrrigationWeb::LinkNeighbor &ln = out.neighbors[out.neighborCount++];
        ln.node = n->num;
        ln.snrQuarterDb = (int8_t)(n->snr * 4);
        ln.hops = n->has_hops_away ? n->hops_away : 0;
        const char *nm = (n->short_name[0]) ? n->short_name : "";
        strncpy(ln.name, nm, sizeof(ln.name) - 1);
        ln.name[sizeof(ln.name) - 1] = 0;
    }
}

// Wizard de 1º boot (§6): grava o papel escolhido e reinicia. Um GATEWAY de fábrica
// gera aqui a PSK AES-256 própria da fazenda (spec §3.2/§6) se o canal ainda não a tem.
// provisioned=true no boot seguinte (config salva) faz o wizard nunca mais reaparecer.
bool IrrigationModule::portalProvision(const IrrigationWeb::ProvisionReq &r)
{
    if (r.role > (uint8_t)IrrigationRole::SERVICO)
        return false;
    settings.role = r.role;
    if (r.role == (uint8_t)IrrigationRole::GATEWAY) {
        meshtastic_Channel ch = channels.getByIndex(channels.getPrimaryIndex());
        if (ch.settings.psk.size != 32) {
            for (int i = 0; i < 32; i++)
                ch.settings.psk.bytes[i] = (uint8_t)random(256);
            ch.settings.psk.size = 32;
        }
        if (r.hasFarmName) {
            memset(ch.settings.name, 0, sizeof(ch.settings.name));
            strncpy(ch.settings.name, r.farmName, sizeof(ch.settings.name) - 1);
        }
        channels.setChannel(ch);
        channels.onConfigChanged();
        service->reloadConfig(SEGMENT_CHANNELS);
    }
    if (!saveIrrigationSettings(settings)) {
        LOG_ERROR("Irrigation: provision failed to persist");
        return false;
    }
    provisioned = true;
    safeMode = false;
    auditEvent(AuditOrigin::SISTEMA, AuditAction::CONFIG_EPOCH, r.role, AuditResult::OK);
    LOG_INFO("Irrigation: provisioned role=%u, rebooting in 3 s", r.role);
    rebootAtMsec = millis() + 3000;
    return true;
}

#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER

void IrrigationModule::otaCycleDecompose(bool &anyValve, bool &anyGpo, bool &groupActive)
{
    // Válvulas locais abertas?
    anyValve = false;
    for (uint8_t i = 0; i < settings.numValves; i++)
        if (valves.isOpen(i)) {
            anyValve = true;
            break;
        }
    // GPOs locais ligados?
    anyGpo = false;
    uint8_t ng = countGpos(settings);
    for (uint8_t i = 0; i < ng; i++)
        if (gpos.isOn(i)) {
            anyGpo = true;
            break;
        }
    // Gateway: grupo hidráulico ou programa em execução?
    groupActive = false;
    if (gwIsGateway()) {
        if (gateway.scheduler.runningProgramId() != 0)
            groupActive = true;
        if (!groupActive) {
            for (size_t gi = 0; gi < gateway.groups.count(); gi++) {
                const HydraulicGroup *grp = gateway.groups.groupAt(gi);
                if (grp && gateway.groupEngine.openConfirmedCount(grp->id) > 0) {
                    groupActive = true;
                    break;
                }
            }
        }
    }
}

bool IrrigationModule::otaCycleActive()
{
    bool anyValve, anyGpo, groupActive;
    otaCycleDecompose(anyValve, anyGpo, groupActive);
    return anyValve || anyGpo || groupActive;
}
#endif // defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER

void IrrigationModule::portalFillOtaStatus(IrrigationWeb::OtaStatusCtx &c)
{
    c.fwVersion = optstr(APP_VERSION);
    const esp_partition_t *part = MeshtasticOTA::getAppPartition();
    c.loaderPresent = (part != nullptr);
    c.loaderBle = false;
    if (part) {
        static esp_app_desc_t desc;
        if (MeshtasticOTA::getAppDesc(part, &desc))
            c.loaderBle = MeshtasticOTA::checkOTACapability(&desc, METHOD_OTA_BLE);
    }
    c.cycleActive = otaCycleActive();
}

bool IrrigationModule::portalOtaArm(const IrrigationWeb::OtaArmReq &r)
{
    // Gate: nunca entrar em OTA com ciclo ativo (o loader não roda o fail-safe de 120 min).
    bool anyValve, anyGpo, groupActive;
    otaCycleDecompose(anyValve, anyGpo, groupActive);
    if (!IrrigationWeb::otaArmAllowed(anyValve, anyGpo, groupActive)) {
        LOG_WARN("Irrigation: OTA recusado — ciclo ativo");
        return false;
    }

    // Preflight do loader.
    const esp_partition_t *part = MeshtasticOTA::getAppPartition();
    if (!part)
        return false;
    static esp_app_desc_t desc;
    if (!MeshtasticOTA::getAppDesc(part, &desc) || !MeshtasticOTA::checkOTACapability(&desc, METHOD_OTA_BLE))
        return false;

    // Defense-in-depth: força-fecha tudo antes de perder o app.
    valves.forceCloseAll();
    gpos.allOff();

    if (!MeshtasticOTA::trySwitchToOTA()) {
        LOG_ERROR("Irrigation: trySwitchToOTA falhou");
        return false;
    }
    MeshtasticOTA::saveConfig(&config.network, meshtastic_OTAMode_OTA_BLE, r.hash);
    auditEvent(AuditOrigin::PAINEL, AuditAction::OTA_ARM, 0, AuditResult::OK);
    LOG_INFO("Irrigation: OTA armado (BLE), reboot no loader em 2 s");
    rebootAtMsec = millis() + 2000;
    return true;
}

#endif // defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER

size_t IrrigationModule::gwBuildAlerts(char *buf, size_t cap)
{
    return IrrigationWeb::buildAlerts(gateway.alerts, millis(), lastAckAllMs, buf, cap);
}

// ---------------------------------------------------------------------------
// Modo Espelhamento UI (Fase X): glue methods para os endpoints CI-only.
// ---------------------------------------------------------------------------

void IrrigationModule::gwSetMirrorEnabled(bool enabled)
{
    gateway.mirror.setEnabled(enabled);
    saveGatewayState();
    auditEvent(AuditOrigin::PAINEL, AuditAction::ESPELHO, 0, AuditResult::OK);
}

bool IrrigationModule::gwApplyMirrorMapping(int8_t input, uint8_t zoneId, bool invertido,
                                            bool habilitado, char *err, size_t errCap)
{
    const Zone *zc = gateway.zones.byId(zoneId);
    if (!zc) {
        snprintf(err, errCap, "zona %u inexistente", zoneId);
        return false;
    }
    // Limpa a porta em qualquer outra zona (1 zona por porta).
    const Zone *other = gateway.zones.byFonte(input);
    if (other && other->id != zoneId) {
        Zone upd = *other;
        upd.fonteInput = -1;
        gateway.zones.upsert(upd);
    }
    Zone z = *zc;
    z.fonteInput = input;
    z.fonteEnabled = habilitado ? 1 : 0;
    if (!gateway.zones.upsert(z)) {
        snprintf(err, errCap, "tabela de zonas cheia");
        return false;
    }
    // Se a associação está sendo pausada (habilitado=false) e o mirror está ativo com
    // esta porta activa, fecha a válvula imediatamente (evita aguardar o fail-safe de 120 s).
    if (!habilitado && gateway.mirror.enabled() && gateway.mirror.inputActive((uint8_t)input)) {
        gwSendValveCmd(z.node, z.index, z.tipo, 0, 0, z.id, 1);
    }
    // Polaridade: bit `input` de digitalInActiveLow nos settings do gateway.
    if (invertido)
        settings.digitalInActiveLow |= (uint8_t)(1u << input);
    else
        settings.digitalInActiveLow &= (uint8_t)~(1u << input);
    saveIrrigationSettings(settings);
    saveGatewayState();
    auditEvent(AuditOrigin::PAINEL, AuditAction::ESPELHO, zoneId, AuditResult::OK);
    return true;
}

bool IrrigationModule::gwDeleteMirrorMapping(int8_t input)
{
    const Zone *z = gateway.zones.byFonte(input);
    if (!z)
        return false;
    uint8_t savedId = z->id;
    // Captura os campos necessários antes de limpar a associação.
    uint32_t savedNode  = z->node;
    uint8_t  savedIndex = z->index;
    uint8_t  savedTipo  = z->tipo;
    int8_t   oldFonteInput = z->fonteInput;
    Zone upd = *z;
    upd.fonteInput = -1;
    gateway.zones.upsert(upd);
    // Se o mirror estava activo com esta porta activa, fecha a válvula imediatamente
    // (evita aguardar o fail-safe de 120 s após a remoção da associação).
    if (gateway.mirror.enabled() && gateway.mirror.inputActive((uint8_t)oldFonteInput)) {
        gwSendValveCmd(savedNode, savedIndex, savedTipo, 0, 0, savedId, 1);
    }
    saveGatewayState();
    auditEvent(AuditOrigin::PAINEL, AuditAction::ESPELHO, savedId, AuditResult::OK);
    return true;
}

size_t IrrigationModule::gwBuildMirror(char *buf, size_t cap)
{
    bool live[4] = {false, false, false, false};
    for (uint8_t i = 0; i < 4 && i < IrrigationSettings::MAX_DIGITAL_IN; i++)
        live[i] = gateway.mirror.inputActive(i);
    return IrrigationWeb::buildMirror(buf, cap, gateway.mirror.enabled(), gateway.zones,
                                      settings.digitalInActiveLow, live);
}

// ---------------------------------------------------------------------------
// Sistema restore — importa tabelas de config de um envelope de backup (§5.5).
// NÃO toca PSK/canal, NÃO reinicializa, NÃO bump de epoch de estação.
// ---------------------------------------------------------------------------

bool IrrigationModule::gwImportTables(const char *json, size_t len, char *resp, size_t respCap)
{
    IrrigationWeb::ImportCounts c;
    char err[48];
    if (!IrrigationWeb::importConfigTablesFromBackup(json, len, gateway.zones, gateway.scheduler,
                                                     gateway.interlocks, gateway.groups,
                                                     c, err, sizeof(err))) {
        snprintf(resp, respCap, "{\"ok\":false,\"err\":\"%s\"}", err);
        return false;
    }
    // Importa a tabela de botões remotos (§5.5 Modo Remoto).
    size_t remoteCount = IrrigationWeb::importRemoteButtonsFromBackup(json, len, gateway.remoteButtons);
    // Persiste as tabelas recém-importadas.
    saveGatewayState();  // zonas + programas (+ stations + mirror — inofensivo, não foram tocados)
    saveInterlocks();    // tabela de intertravamentos (arquivo separado)
    saveGroups();        // tabela de grupos hidráulicos (arquivo separado)
    saveRemoteButtons(); // associações botoeira→zona (§5.5 Modo Remoto)
    // §8.9: audita importação de configuração — CONFIG_EPOCH é a ação existente mais próxima
    // de "substituição em bloco das tabelas de config via painel".
    auditEvent(AuditOrigin::PAINEL, AuditAction::CONFIG_EPOCH, 0, AuditResult::OK);
    snprintf(resp, respCap,
             "{\"ok\":true,\"zonas\":%u,\"programas\":%u,\"intertravamentos\":%u,\"grupos\":%u,\"remoteButtons\":%u}",
             c.zonas, c.programas, c.intertravamentos, c.grupos, (unsigned)remoteCount);
    return true;
}

bool IrrigationModule::portalPulse(const IrrigationWeb::PortalPulseReq &p)
{
    // Teste de pulso local: abre a válvula com fechamento automático pelo timer fail-safe.
    bool ok = valves.open(p.valveId, p.durationS, settings.maxOpenConfigS, millis()) == ValveController::Result::OK;
    // §8.9: audita pulso solicitado pelo portal de campo.
    auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::PULSO, p.valveId, ok ? AuditResult::OK : AuditResult::NACK);
    if (!ok)
        return false;
    sendEvento(EV_TEST_PULSE);
    return true;
}

bool IrrigationModule::portalRunNetCommand(const IrrigationWeb::NetCommand &c)
{
    // Ação desconhecida (só 0=fechar, 1=abrir): rejeita — não trata silenciosamente como fechar.
    if (c.action != 0 && c.action != 1) {
        auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::CMD_REJEITADO, REASON_BAD_PAYLOAD, AuditResult::NACK);
        return false;
    }
    if (gwIsGateway()) {
        // Modo seguro: não abre (mesmo intertravamento de handleCmdValvula). Fechar continua permitido.
        if (safeMode && c.action == 1) {
            auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::ABRIR, c.zoneId, AuditResult::NACK);
            return false;
        }
        // Este nó é o gateway: aplica local sem rádio (reusa a validação de zona do gateway).
        const Zone *z = gateway.zones.byId(c.zoneId);
        if (!z) {
            auditEvent(AuditOrigin::PORTAL_CAMPO, c.action == 1 ? AuditAction::ABRIR : AuditAction::FECHAR,
                       c.zoneId, AuditResult::NACK);
            return false;
        }
        const StationEntry *st = gateway.stations.byNode(z->node);
        uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
        if (c.action == 1) {
            // Fase 6b: bloqueia abertura via portal se intertravamento está ativo para esta zona.
            if (gateway.interlockEngine.zoneVerdict(z->id).bloqueada) {
                auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::CMD_REJEITADO, z->id, AuditResult::NACK, z->node);
                return false;
            }
            uint16_t dur = c.durationS;
            if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60))
                dur = (uint16_t)(z->maxMin * 60);
            if (routeZoneToGroup(z->id, true, dur)) {
                auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::ABRIR, c.zoneId, AuditResult::OK);
                return true;
            }
            gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts);
        } else {
            if (routeZoneToGroup(z->id, false, 0)) {
                auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::FECHAR, c.zoneId, AuditResult::OK);
                return true;
            }
            gateway.openGate.release(z->id); // balanceia a contagem de simultaneidade (no-op se não estava aberta)
            gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
        }
        // §8.9: audita comando de rede via portal (gateway aplica localmente).
        auditEvent(AuditOrigin::PORTAL_CAMPO, c.action == 1 ? AuditAction::ABRIR : AuditAction::FECHAR,
                   c.zoneId, AuditResult::OK);
        return true;
    }
    // Nó de campo: encaminha ao gateway vinculado por rádio.
    if (settings.boundGateway == 0) {
        auditEvent(AuditOrigin::PORTAL_CAMPO, c.action == 1 ? AuditAction::ABRIR : AuditAction::FECHAR,
                   c.zoneId, AuditResult::NACK);
        return false;
    }
    IrrigationProto::RemoteCmd m = {};
    m.zoneId = c.zoneId;
    m.action = c.action;
    m.durationS = c.durationS;
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = settings.boundGateway;
    p->decoded.payload.size =
        (uint16_t)IrrigationProto::encodeRemoteCmd(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, m);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        // §8.9: falha de encode = rejeição local.
        auditEvent(AuditOrigin::PORTAL_CAMPO, c.action == 1 ? AuditAction::ABRIR : AuditAction::FECHAR,
                   c.zoneId, AuditResult::NACK);
        return false;
    }
    txPacket(p);
    // §8.9: TX despachado ao gateway — OK = rádio TX enviado (sem confirmação end-to-end aqui).
    auditEvent(AuditOrigin::PORTAL_CAMPO, c.action == 1 ? AuditAction::ABRIR : AuditAction::FECHAR,
               c.zoneId, AuditResult::OK);
    return true;
}

void IrrigationModule::portalFillSensors(IrrigationWeb::PortalSensorsCtx &out) const
{
    IrrigationProto::SensorReading rs[IrrigationSettings::MAX_SENSORS];
    size_t n = sampler.readings(rs);
    out.count = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        out.items[i].id = rs[i].id;
        out.items[i].tipo = rs[i].tipo;
        out.items[i].valueCenti = rs[i].valueCenti;
        // unidade vem do slot correspondente no pin map (rs[i].id = índice do slot)
        out.items[i].unidade = (rs[i].id < IrrigationSettings::MAX_SENSORS) ? settings.sensores[rs[i].id].unidade : 0;
    }
}

bool IrrigationModule::portalGpo(const IrrigationWeb::PortalGpoReq &r)
{
    // Intertravamento: modo seguro nunca liga (mesma regra de handleCmdGpo/portalPulse).
    if (r.action == 1 && safeMode) {
        auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::CMD_REJEITADO, REASON_SAFE_MODE, AuditResult::NACK);
        return false;
    }
    // Biestável (durationS==0 ao ligar) exige confirmação extra da UI (§8.11).
    if (r.action == 1 && r.durationS == 0 && !r.confirm) {
        auditEvent(AuditOrigin::PORTAL_CAMPO, AuditAction::CMD_REJEITADO, REASON_BAD_PAYLOAD, AuditResult::NACK);
        return false;
    }
    GpoController::Result res = gpos.command(r.gpoId, r.action, r.durationS, millis());
    bool ok = (res == GpoController::Result::OK);
    auditEvent(AuditOrigin::PORTAL_CAMPO, r.action ? AuditAction::GPO_ON : AuditAction::GPO_OFF, r.gpoId,
               ok ? AuditResult::OK : AuditResult::NACK);
    return ok;
}

void IrrigationModule::portalGetCoords(IrrigationWeb::PortalCoords &out) const
{
    out.latE7 = settings.latE7;
    out.lonE7 = settings.lonE7;
}

bool IrrigationModule::portalSetCoords(const IrrigationWeb::PortalCoords &c)
{
    settings.latE7 = c.latE7;
    settings.lonE7 = c.lonE7;
    // Coordenada é local (§8.8): NÃO incrementa config_epoch.
    return saveIrrigationSettings(settings);
}

bool IrrigationModule::routeZoneToGroup(uint8_t zoneId, bool open, uint16_t durationS)
{
    const HydraulicGroup *hg = gateway.groups.byZone(zoneId);
    if (!hg)
        return false; // zona livre — chamador segue caminho direto/OpenGate
    if (open) {
        // Intertravamento: zona bloqueada não entra no desejado (mesma política do scheduler).
        if (gateway.interlockEngine.zoneVerdict(zoneId).bloqueada) {
            const Zone *z = gateway.zones.byId(zoneId);
            auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::CMD_REJEITADO, zoneId,
                       AuditResult::NACK, z ? z->node : 0);
        } else {
            gateway.groupEngine.setDesired(hg->id, zoneId, true, durationS);
        }
    } else {
        gateway.groupEngine.setDesired(hg->id, zoneId, false, 0);
    }
    return true;
}

// Decisão §2: loop principal do gateway — scheduler, espelho, retries, silêncio.
void IrrigationModule::gwTick()
{
    // Drena ACKs sintéticos do drive local: confirma DEPOIS que o loop de emissão
    // do motor de grupos registrou noteSent (evita reentrância).
    for (uint8_t i = 0; i < pendingLocalAckCount; i++)
        confirmCommand(pendingLocalAck[i].node, pendingLocalAck[i].seq, 0 /*reason*/, true /*ok*/);
    pendingLocalAckCount = 0;

    // --- Fase 6b: motor de intertravamentos — avaliação 1×/tick, antes do scheduler ---
    {
        // Monta snapshot de sensores a partir do cache de telemetria das estações.
        SensorSnapshot snaps[StationTelemetryCache::MAX * IrrigationProto::HB_MAX_SENSORS];
        size_t ns = 0;
        for (size_t i = 0; i < StationTelemetryCache::MAX; i++) {
            const StationTelemetry *t = gateway.telemetry.entryAt(i);
            if (!t || !t->node)
                continue;
            for (uint8_t k = 0; k < t->sensorCount && k < IrrigationProto::HB_MAX_SENSORS; k++)
                snaps[ns++] = SensorSnapshot{t->node, t->sensors[k].id, true,
                                             t->sensors[k].valueCenti != 0, (int32_t)t->sensors[k].valueCenti};
        }
        // Avalia regras; cap == 0 = sem limite de simultaneidade.
        uint8_t cap = gateway.interlockEngine.evaluate(gateway.interlocks, snaps, ns);
        gateway.openGate.setCap(cap);

        // fechar_e_bloquear: fecha zona na BORDA DE SUBIDA (evita spam rádio por tick).
        // interlockClosedMask é um bitmap de 256 bits indexado por zoneId.
        for (size_t i = 0; i < gateway.zones.count(); i++) {
            const Zone *z = gateway.zones.zoneAt(i);
            if (!z)
                continue;
            ZoneVerdict v = gateway.interlockEngine.zoneVerdict(z->id);
            uint8_t byteIdx = z->id >> 3;
            uint8_t bitMask = (uint8_t)(1u << (z->id & 0x07));
            bool jaFechada = (interlockClosedMask[byteIdx] & bitMask) != 0;
            if (v.deveFechar) {
                if (!jaFechada) {
                    // Fase 7a: zona de grupo hidráulico é fechada PELO MOTOR (sequência bomba-off
                    // -> válvula, ordem hidráulica correta). Fechar direto na estação faria o motor
                    // reabri-la no próximo tick (wanted ainda true), quicando o intertravamento.
                    const HydraulicGroup *hg = gateway.groups.byZone(z->id);
                    if (hg) {
                        gateway.groupEngine.setDesired(hg->id, z->id, false, 0);
                    } else {
                        // Borda de subida: envia FECHAR direto (zona não agrupada).
                        gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
                    }
                    auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::FECHAR, z->id, AuditResult::OK, z->node);
                    interlockClosedMask[byteIdx] |= bitMask;
                }
            } else {
                // Intertravamento desarmado: libera para reabrir.
                interlockClosedMask[byteIdx] &= (uint8_t)(~bitMask);
            }
        }
    }

    // --- Controle de nível por boia (enchimento) — após intertravamentos ---
    gwLevelTick(millis());

    // --- Scheduler (programa/cronograma) ---
    // RTC-OPTIONAL: se não há RTC válido, idle com LOG_WARN 1×/h.
    uint32_t epochLocal = 0;
    bool hasRtc = computeLocalSecs(epochLocal);
    if (!hasRtc) {
        if (millis() - lastRtcWarnMs > 3600000U) {
            LOG_WARN("Irrigation GW: no valid RTC — scheduler idle");
            lastRtcWarnMs = millis();
        }
    } else {
        // Drena ações do scheduler até NONE.
        for (;;) {
            SchedAction a = gateway.scheduler.tick(epochLocal);
            if (a.type == SchedAction::Type::NONE)
                break;
            const Zone *z = gateway.zones.byId(a.zoneId);
            if (!z) {
                LOG_WARN("Irrigation GW: scheduler zone %u not found", a.zoneId);
                continue;
            }
            // --- Gate de supressão climática (Open-Meteo) — fail-open por design ---
            if (a.type == SchedAction::Type::OPEN) {
                WeatherVerdict wv = weatherVerdictForZone(a.zoneId);
                if (wv.suppress) {
                    LOG_INFO("Irrigation GW: OPEN zona=%u suprimido por clima (regra %u)", a.zoneId, wv.ruleId);
                    auditEvent(AuditOrigin::CLIMA, AuditAction::CMD_SUPRIMIDO, a.zoneId, AuditResult::OK, z->node);
                    continue; // não abre; próximo tick reavalia (fail-open embutido)
                }
            }
            // --- Fase 7a/7b: zonas de grupo são orquestradas pelo motor (roteamento único) ---
            if (a.type == SchedAction::Type::OPEN) {
                uint16_t durG = a.durationS;
                if (z->maxMin > 0 && durG > (uint16_t)(z->maxMin * 60))
                    durG = (uint16_t)(z->maxMin * 60);
                if (routeZoneToGroup(a.zoneId, true, durG))
                    continue;
            } else { // CLOSE
                if (routeZoneToGroup(a.zoneId, false, 0))
                    continue;
            }
            if (a.type == SchedAction::Type::OPEN) {
                // Bypass: se o espelho é dono desta zona, ele manda — suprime o OPEN.
                if (mirrorOwnsZoneOutput(gateway.mirror, z->fonteInput, z->fonteEnabled)) {
                    LOG_DEBUG("Irrigation GW: scheduler OPEN zone=%u suprimido (espelho dono)", a.zoneId);
                    continue;
                }
                // Clamp pela maxMin da zona (scheduler já conhece durationS).
                uint16_t dur = a.durationS;
                if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60))
                    dur = (uint16_t)(z->maxMin * 60);
                // Retries: obtém da entrada de registro da estação (default 3).
                const StationEntry *stEntry = gateway.stations.byNode(z->node);
                uint8_t attempts = (stEntry && stEntry->retries > 0) ? stEntry->retries : 3;
                // Fase 6b: verifica bloqueio por intertravamento antes de abrir.
                ZoneVerdict sv = gateway.interlockEngine.zoneVerdict(a.zoneId);
                if (sv.bloqueada) {
                    auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::CMD_REJEITADO, a.zoneId,
                               AuditResult::NACK, z->node);
                    // Não abre; se a regra persistir, continuará bloqueado no próximo tick.
                } else if (gateway.openGate.request(a.zoneId, dur) == OpenGate::Decision::ADMIT) {
                    gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, a.zoneId, attempts, AuditOrigin::CRONOGRAMA);
                }
                // HOLD: enfileirado com dur clampeado; dreno abaixo libera quando houver capacidade.
            } else { // CLOSE
                // Mesma proteção: não feche o que o espelho mantém aberto (carryover F4 #1).
                if (mirrorOwnsZoneOutput(gateway.mirror, z->fonteInput, z->fonteEnabled)) {
                    LOG_DEBUG("Irrigation GW: scheduler CLOSE zone=%u suprimido (espelho dono)", a.zoneId);
                    continue;
                }
                gateway.openGate.release(a.zoneId);
                gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, a.zoneId, 1);
            }
        }
    }

    // --- Supressão meteorológica: tick do client (2×/dia + seed pós-boot) ---
    {
#if defined(ARCH_ESP32)
        bool staUp = WiFi.isConnected();
#else
        bool staUp = false;
#endif
        if (weatherClient.tick(gateway.weatherConfig, epochLocal, staUp, gateway.weatherCache))
            LOG_INFO("Weather: cache atualizado (12h=%u cmm, prob=%u%%)",
                     gateway.weatherCache.chuvaPrevista12hCenti, gateway.weatherCache.probChuvaPct);
    }

    // --- Fase 6b: drena a fila de simultaneidade enquanto houver capacidade ---
    {
        OpenGate::Pending p;
        while ((p = gateway.openGate.nextAdmittable()).zoneId != 0) {
            const Zone *qz = gateway.zones.byId(p.zoneId);
            if (!qz)
                continue; // zona removida enquanto na fila
            if (gateway.interlockEngine.zoneVerdict(p.zoneId).bloqueada) {
                auditEvent(AuditOrigin::INTERTRAVAMENTO, AuditAction::CMD_REJEITADO, p.zoneId, AuditResult::NACK, qz->node);
                continue; // bloqueou nesse meio-tempo — descarta da fila
            }
            gateway.openGate.request(p.zoneId, p.durationS); // registra a abertura no slot
            gwSendValveCmd(qz->node, qz->index, qz->tipo, 1, p.durationS, p.zoneId, 1);
        }
    }

    // --- Fase 7a: motor de grupos hidráulicos ---
    {
        GroupEmit emits[HydraulicGroupEngine::MAX_GROUPS * 2];
        size_t ne = gateway.groupEngine.tick(gateway.groups, gateway.zones, millis(), emits,
                                             sizeof(emits) / sizeof(emits[0]));
        for (size_t i = 0; i < ne; i++) {
            const GroupEmit &em = emits[i];
            const StationEntry *st = gateway.stations.byNode(em.node);
            uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
            uint32_t seq = gwSendValveCmd(em.node, em.index, em.tipo, em.action, em.durationS, em.zoneId, attempts,
                                          AuditOrigin::GRUPO_HIDRAULICO);
            if (seq == 0) {
                // encode/alloc falhou: tick() já armou pend.inUse=true; limpa para que o próximo tick reemita
                gateway.groupEngine.onCmdFailed(em.node, em.zoneId, em.action);
                continue;
            }
            gateway.groupEngine.noteSent(em.node, em.zoneId, em.action, seq);
            auditEvent(AuditOrigin::GRUPO_HIDRAULICO, em.action ? AuditAction::ABRIR : AuditAction::FECHAR,
                       em.zoneId, AuditResult::OK, em.node);
        }
        HydraulicGroupEngine::GroupAlert ga;
        while (gateway.groupEngine.takeAlert(ga)) {
            Alert al;
            al.type = AlertType::CMD_FAIL;
            al.node = 0;
            al.arg = ga.code;
            al.atMs = millis();
            gateway.alerts.push(al);
            auditEvent(AuditOrigin::GRUPO_HIDRAULICO, AuditAction::CMD_REJEITADO, ga.zoneId, AuditResult::NACK, 0);
        }
    }

    // --- Espelho (mirror) ---
    // Lê GPIO das entradas digitais com polaridade e monta bitmap.
    uint8_t rawBitmap = 0;
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
        if (settings.pinsDigitalIn[i] < 0)
            continue;
#ifndef ARCH_PORTDUINO
        bool raw = (digitalRead(settings.pinsDigitalIn[i]) == HIGH);
#else
        bool raw = false; // nativo: sem GPIO real
#endif
        bool activeLow = (settings.digitalInActiveLow >> i) & 1;
        if (activeLow ? !raw : raw)
            rawBitmap |= (1u << i);
    }
    // Drena ações do mirror até NONE.
    for (;;) {
        MirrorMode::Action ma = gateway.mirror.update(rawBitmap, millis());
        if (ma.t == MirrorMode::Action::T::NONE)
            break;
        const Zone *z = gateway.zones.byFonte(ma.input);
        if (!z) {
            LOG_DEBUG("Irrigation GW: mirror input %u has no zone mapped — ignored", ma.input);
            continue;
        }
        if (!z->fonteEnabled)
            continue; // associação pausada: espelho não comanda; scheduler controla
        if (ma.t == MirrorMode::Action::T::OPEN) {
            // bypass total: sem clamp de maxMin (estação clampa no teto compilado)
            const StationEntry *stEntry = gateway.stations.byNode(z->node);
            uint8_t attempts = (stEntry && stEntry->retries > 0) ? stEntry->retries : 3;
            gwSendValveCmd(z->node, z->index, z->tipo, 1, MirrorMode::OPEN_S, z->id, attempts);
        } else {
            gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
        }
    }

    // --- Botoeira local do gateway (Modo Remoto Task 6 Step 6) ---
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
        if (settings.pinsDigitalIn[i] < 0 || !digitalInIsButton(settings, i))
            continue;
#ifndef ARCH_PORTDUINO
        bool raw = (digitalRead(settings.pinsDigitalIn[i]) == HIGH);
#else
        bool raw = false;
#endif
        bool activeLow = (settings.digitalInActiveLow >> i) & 1;
        bool pressed = activeLow ? !raw : raw;
        if (_btnEdge[i].update(pressed, millis())) {
            uint8_t slot = digitalInLedSlot(settings, i);
            if (slot <= 1)
                _remoteLed[slot].onPress(millis());
            gwFireRemote(nodeDB->getNodeNum(), i);
        }
    }

    // --- Retries (tracker) ---
    for (;;) {
        CommandTracker::Retry r = gateway.tracker.poll(millis());
        if (r.what == CommandTracker::Retry::What::NONE)
            break;
        if (r.what == CommandTracker::Retry::What::RESEND) {
            LOG_DEBUG("Irrigation GW: RESEND node=0x%08x zone=%u attempts_left=%u", r.node, r.zoneId, r.attemptsLeft);
            const Zone *z = gateway.zones.byId(r.zoneId);
            uint8_t idx = z ? z->index : 0;
            uint8_t tp = z ? z->tipo : 0;
            meshtastic_MeshPacket *p = allocDataPacket();
            p->to = r.node;
            if (tp == 1) {
                CmdGpo cmd = {};
                cmd.gpoId = idx;
                cmd.action = r.action;
                cmd.durationS = r.durationS;
                p->decoded.payload.size = (uint16_t)encodeCmdGpo(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, cmd);
            } else {
                CmdValvula cmd = {};
                cmd.valveId = idx;
                cmd.action = r.action;
                cmd.durationS = r.durationS;
                p->decoded.payload.size = (uint16_t)encodeCmdValvula(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, cmd);
            }
            if (!p->decoded.payload.size) {
                packetPool.release(p);
                gateway.alerts.push({AlertType::CMD_FAIL, r.node, 0xFF, millis()}); // 0xFF = falha interna de encode
            } else {
                txPacket(p);
                gateway.tracker.retrack(txSeq, r, millis());
            }
        } else { // FAILED
            LOG_WARN("Irrigation GW: CMD_FAIL node=0x%08x zone=%u — alerting", r.node, r.zoneId);
            Alert a;
            a.type = AlertType::CMD_FAIL;
            a.node = r.node;
            a.arg = r.zoneId;
            a.atMs = millis();
            gateway.alerts.push(a);
            // Fase 7a: notifica o motor de grupos p/ tratar falha de comando da zona.
            gateway.groupEngine.onCmdFailed(r.node, r.zoneId, r.action);
        }
    }

    // --- Silêncio por estação (itera o registry diretamente, não em paralelo à allowlist) ---
    for (size_t i = 0; i < gateway.stations.count(); i++) {
        const StationEntry *entry = gateway.stations.nodeAt(i);
        if (!entry)
            break;
        uint32_t silMs = (uint32_t)entry->silencioAlertaMin * 60000UL;
        Alert a;
        if (gateway.monitor.checkSilence(entry->node, silMs, millis(), a)) {
            gateway.alerts.push(a);
            LOG_WARN("Irrigation GW: SILENT node=0x%08x", entry->node);
        }
    }

    // Modo Remoto: refresh periódico dos LEDs de estado compartilhado.
    // Garante que alterações por scheduler, grupos, fail-safe ou heartbeat de estação
    // repintem a botoeira-nó corretamente. gwPushRemoteLed() é change-driven (_ledCache
    // suprime RF quando o bitmap não mudou), então chamar a cada tick é barato.
    if (gateway.remoteButtons.count() > 0)
        gwPushRemoteLed();
}

// Preenche os campos de rota de fallback P2P de `cfg` (config de UMA estação `stationNode`)
// a partir das associações de botoeira + zonas + grupos do gateway. Chamado no momento do
// push de SET_CONFIG. Deixa campos zerados/"nenhum" quando não há fallback (zona grupo,
// zona ausente, ou entrada sem associação).
void IrrigationModule::gwOverlayFallbackRoute(uint32_t stationNode, IrrigationSettings &cfg)
{
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++) {
        // default: sem fallback / "nenhum"
        cfg.btnFallbackNode[i] = 0;
        cfg.btnFallbackOutId[i] = 0;
        cfg.btnFallbackKind = (uint8_t)((cfg.btnFallbackKind & ~(0x3u << (2 * i))) | (0x3u << (2 * i)));
        const RemoteAssoc *hits[RemoteButtonTable::MAX];
        size_t k = gateway.remoteButtons.findByTrigger(stationNode, i, hits, RemoteButtonTable::MAX);
        if (k == 0)
            continue;
        const RemoteAssoc *a = hits[0];
        if (!a->enabled)
            continue;
        const Zone *z = gateway.zones.byId(a->targetZoneId);
        if (!z)
            continue;
        bool isGroup = (gateway.groups.byZone(z->id) != nullptr);
        bool fwOk = true; // TODO(follow-up): gate on target node survey APP_FW_VERSION>=0x0900.
                          // Old node receiving action==2 falls into the close path (safe direction).
        FallbackRoute r = compileFallbackRoute(z->node, z->index, z->tipo, isGroup, fwOk);
        if (r.node == 0)
            continue; // grupo/zona inválida → mantém "nenhum"
        cfg.btnFallbackNode[i] = r.node;
        cfg.btnFallbackOutId[i] = r.outputId;
        cfg.btnFallbackKind = (uint8_t)((cfg.btnFallbackKind & ~(0x3u << (2 * i))) | ((r.kind & 0x3u) << (2 * i)));
    }
    cfg.remoteFallbackMs = 0; // usa o default compilado (REMOTE_FALLBACK_DEFAULT_MS)
}

// Decisão §3: reconciliação de epoch com cooldown de 30 s.
void IrrigationModule::gwReconcileEpoch(uint32_t node, uint32_t remoteEpoch)
{
    const StationEntry *entry = gateway.stations.byNode(node);
    if (!entry)
        return;

    // Fix 1 (guard §5.4): blob só é válido após adoptConfig; nunca empurrar zeros.
    if (entry->desiredEpoch == 0)
        return; // blob só é válido após adoptConfig; nunca empurrar zeros (§5.4)

    // Fix 3: cooldown indexado por nó (não por posição na allowlist).
    // Localiza slot existente ou toma o mais antigo (find-or-create).
    uint8_t slot = 0xFF;
    uint8_t oldestSlot = 0;
    uint32_t oldestMs = epochCooldowns[0].lastMs;
    for (uint8_t i = 0; i < StationRegistry::MAX; i++) {
        if (epochCooldowns[i].node == node) {
            slot = i;
            break;
        }
        if (epochCooldowns[i].node == 0 && slot == 0xFF) {
            slot = i; // preferência por slot vazio
            break;
        }
        if (epochCooldowns[i].lastMs < oldestMs) {
            oldestMs = epochCooldowns[i].lastMs;
            oldestSlot = i;
        }
    }
    if (slot == 0xFF)
        slot = oldestSlot; // todos ocupados: recicla o mais antigo
    epochCooldowns[slot].node = node;

    uint32_t now = millis();
    if (now - epochCooldowns[slot].lastMs < EPOCH_COOLDOWN_MS)
        return; // cooldown ainda ativo

    if (remoteEpoch < entry->desiredEpoch) {
        // Estação está atrás: envia SET_CONFIG com o blob desejado.
        epochCooldowns[slot].lastMs = now;
        // P2P fallback: sobrepõe a rota compilada nesta cópia local antes de calcular o CRC.
        IrrigationSettings _cfgTx;
        const uint8_t *blob;
        uint16_t totalLen = (uint16_t)sizeof(entry->blob);
        if (migrateIrrigationSettings(entry->blob, sizeof(entry->blob), _cfgTx)) {
            gwOverlayFallbackRoute(entry->node, _cfgTx);
            blob = (const uint8_t *)&_cfgTx;
        } else {
            blob = entry->blob; // blob inválido: empurra como está (comportamento anterior)
        }
        uint32_t crc = crc32(blob, totalLen);
        uint8_t fragCount = (uint8_t)((totalLen + FRAG_DATA_MAX - 1) / FRAG_DATA_MAX);
        for (uint8_t i = 0; i < fragCount; i++) {
            SetConfig sc = {};
            sc.epoch = entry->desiredEpoch;
            sc.crc = crc;
            sc.totalLen = totalLen;
            sc.fragIndex = i;
            sc.fragCount = fragCount;
            uint16_t off = (uint16_t)i * FRAG_DATA_MAX;
            sc.fragLen = (uint8_t)((totalLen - off > FRAG_DATA_MAX) ? FRAG_DATA_MAX : (uint8_t)(totalLen - off));
            sc.frag = blob + off;
            meshtastic_MeshPacket *p = allocDataPacket();
            p->to = node;
            p->decoded.payload.size =
                (uint16_t)encodeSetConfig(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, sc);
            if (!p->decoded.payload.size) {
                packetPool.release(p);
                return;
            }
            txPacket(p);
        }
        LOG_INFO("Irrigation GW: pushed config epoch=%u to node=0x%08x", entry->desiredEpoch, node);
    } else if (remoteEpoch > entry->desiredEpoch) {
        // Estação está à frente do desejado: solicita GET_CONFIG para adotar.
        epochCooldowns[slot].lastMs = now;
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = node;
        p->decoded.payload.size = (uint16_t)encodeGetConfig(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq);
        if (!p->decoded.payload.size) {
            packetPool.release(p);
            return;
        }
        txPacket(p);
        LOG_INFO("Irrigation GW: GET_CONFIG from node=0x%08x (remote epoch=%u > desired=%u)", node, remoteEpoch,
                 entry->desiredEpoch);
    }
    // remoteEpoch == desiredEpoch: em sincronia, nada a fazer.
}

void IrrigationModule::handleRemoteCmd(const meshtastic_MeshPacket &mp, const Header &h)
{
    // Autoridade = posse da PSK da fazenda (spec §7.2); anti-replay por seq ainda vale.
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation GW: replayed remote-cmd seq %u from 0x%08x", h.seq, mp.from);
        return;
    }
    if (!rateLimiter.allow(millis())) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_RATE_LIMIT);
        return;
    }
    RemoteCmd cmd;
    if (!decodeRemoteCmd(mp.decoded.payload.bytes, mp.decoded.payload.size, cmd)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        return;
    }
    IrrigationWeb::NetCommand nc = {};
    nc.zoneId = cmd.zoneId;
    nc.action = cmd.action;
    nc.durationS = cmd.durationS;
    bool ok = portalRunNetCommand(nc); // gateway => aplica local
    sendAck(mp.from, h.seq, ok ? ACK_OK : ACK_NACK, ok ? REASON_NONE : REASON_INVALID_ID);
}

// Decisão §3: MSG_ACK recebido pelo gateway.
void IrrigationModule::confirmCommand(uint32_t node, uint32_t seq, uint8_t reason, bool ok)
{
    if (!ok) {
        // Task 6, decisão §6: NACK é resposta definitiva — remove pendência e alerta.
        // onAck remove a pendência independente do status.
        if (gateway.tracker.onAck(node, seq)) {
            LOG_WARN("Irrigation GW: NACK from 0x%08x seq=%u reason=%u", node, seq, reason);
            // Determina zoneId a partir do ackedSeq — o tracker já removeu o slot,
            // então logamos com zoneId=0 (informação de alerta é best-effort aqui;
            // o diagnóstico detalhado é Fase 6).
            Alert a;
            a.type = AlertType::CMD_FAIL;
            a.node = node;
            a.arg = reason; // reason como arg conforme decisão §6
            a.atMs = millis();
            gateway.alerts.push(a);
        }
        // Fase 7a: NACK é FALHA — o motor de grupos NÃO deve avançar como se tivesse ligado.
        gateway.groupEngine.onNack(node, seq);
    } else {
        // Peek zoneId ANTES de onAck remover o slot (para gwPushRemoteLed abaixo).
        uint8_t ackZoneId = 0, ackAction_ = 0; // ackAction_ não usado; peek precisa do out-param
        bool hadPending = gateway.tracker.peekZone(node, seq, ackZoneId, ackAction_);
        (void)ackAction_;
        gateway.tracker.onAck(node, seq);
        // Fase 7a: só o ACK OK avança o handshake do motor de grupos.
        gateway.groupEngine.onAck(node, seq);
        // Modo Remoto: atualiza LED de feedback quando o estado de zona muda por qualquer origem.
        if (hadPending && ackZoneId != 0 && gateway.remoteButtons.count() > 0)
            gwPushRemoteLed();
    }
}

void IrrigationModule::handleGwAck(const meshtastic_MeshPacket &mp, const Header &h)
{
    Ack ack;
    if (!decodeAck(mp.decoded.payload.bytes, mp.decoded.payload.size, ack)) {
        LOG_WARN("Irrigation GW: bad ACK payload from 0x%08x", mp.from);
        return;
    }

    // Modo Remoto (Task 6 fix): ACK traz o estado pós-comando das saídas — atualiza cache
    // ANTES de confirmCommand para que gwZoneIsOpen (chamado internamente via gwPushRemoteLed)
    // já enxergue o estado real pós-comando quando calcula o bitmap de LED.
    {
        const StationTelemetry *existing = gateway.telemetry.byNode(mp.from);
        StationTelemetry tel = existing ? *existing : StationTelemetry{};
        tel.node = mp.from;
        tel.valveStates = ack.valveStates;
        tel.gpoStates = ack.gpoStates;
        gateway.telemetry.update(tel);
    }

    confirmCommand(mp.from, ack.ackedSeq, ack.reason, ack.status == ACK_OK);

    // Reconciliação de epoch (mesma regra do HB — decisão §3).
    gwReconcileEpoch(mp.from, ack.configEpoch);
}

// Decisão §3: MSG_HEARTBEAT recebido pelo gateway.
void IrrigationModule::handleGwHeartbeat(const meshtastic_MeshPacket &mp, const Header &h)
{
    Heartbeat hb;
    if (!decodeHeartbeat(mp.decoded.payload.bytes, mp.decoded.payload.size, hb)) {
        LOG_WARN("Irrigation GW: bad HB payload from 0x%08x", mp.from);
        return;
    }

    // Alertas de bateria e reboot via StationMonitor.
    Alert monAlerts[3];
    int cnt = gateway.monitor.onHeartbeat(mp.from, hb.vbatCentiV, hb.rebootCount, millis(), monAlerts);
    for (int i = 0; i < cnt; i++)
        gateway.alerts.push(monAlerts[i]);

    // Reconciliação de epoch.
    gwReconcileEpoch(mp.from, hb.configEpoch);

    // Telemetria por estação (RAM; alimenta o painel web).
    StationTelemetry tel = {};
    tel.node = mp.from;
    tel.vbatCentiV = hb.vbatCentiV;
    tel.vpanelCentiV = hb.vpanelCentiV;
    tel.snrQuarterDb = hb.snrQuarterDb;
    tel.rssiDbm = hb.rssi;
    tel.rebootCount = hb.rebootCount;
    tel.flags = hb.flags;
    tel.configEpoch = hb.configEpoch;
    tel.atMs = millis();
    // Fase 6b (Task 11): copia bloco de sensores e bit tamper do heartbeat para o cache.
    tel.sensorCount = hb.sensorCount;
    for (uint8_t i = 0; i < hb.sensorCount && i < IrrigationProto::HB_MAX_SENSORS; i++)
        tel.sensors[i] = hb.sensors[i];
    tel.tamper = (hb.flags & IrrigationProto::HB_FLAG_TAMPER) != 0;
    // Modo Remoto (Task 6 fix): estado real das saídas — alimenta gwZoneIsOpen para zonas remotas.
    tel.valveStates = hb.valveStates;
    tel.gpoStates = hb.gpoStates;
    gateway.telemetry.update(tel);

    // Fase 7a: reporta o estado real das saídas (bitmaps do HB) ao motor de grupos.
    for (size_t i = 0; i < gateway.zones.count(); i++) {
        const Zone *zz = gateway.zones.zoneAt(i);
        if (!zz || zz->node != mp.from)
            continue;
        const HydraulicGroup *hg = gateway.groups.byZone(zz->id);
        if (!hg)
            continue;
        bool open = (zz->tipo == 1) ? ((hb.gpoStates >> zz->index) & 1)
                                    : ((hb.valveStates >> zz->index) & 1);
        gateway.groupEngine.observeActual(hg->id, zz->id, open);
    }
}

// Decisão §3: MSG_EVENTO recebido pelo gateway (auditoria; Fase 6 processa detalhes).
void IrrigationModule::handleGwEvento(const meshtastic_MeshPacket &mp, const Header &h)
{
    Evento ev;
    if (!decodeEvento(mp.decoded.payload.bytes, mp.decoded.payload.size, ev)) {
        LOG_WARN("Irrigation GW: bad EVENTO from 0x%08x", mp.from);
        return;
    }
    LOG_INFO("Irrigation GW: EVENTO code=%u arg=%u from 0x%08x (audit log)", ev.code, ev.arg, mp.from);
}

// Decisão §3: MSG_SET_CONFIG no role GATEWAY = resposta de GET_CONFIG.
void IrrigationModule::handleGwSetConfig(const meshtastic_MeshPacket &mp, const Header &h)
{
    SetConfig sc;
    if (!decodeSetConfig(mp.decoded.payload.bytes, mp.decoded.payload.size, sc)) {
        LOG_WARN("Irrigation GW: bad SET_CONFIG from 0x%08x", mp.from);
        return;
    }

    auto r = reasm.add(mp.from, sc.epoch, sc.crc, sc.totalLen, sc.fragIndex, sc.fragCount, sc.frag, sc.fragLen, millis());
    if (r != FragmentReassembler::Add::COMPLETE)
        return; // intermediário ou inválido: sem ação por enquanto

    // Blob completo: adoptar como config da estação (adoptConfig migra p/ v5 canônico).
    if (reasm.blobLen() <= sizeof(StationEntry::blob)) {
        gateway.stations.adoptConfig(mp.from, reasm.blob(), reasm.blobLen(), reasm.epoch());
        saveGatewayState(); // persiste após mutação (decisão §5)
        gwRebuildLocalInterlocks(); // Fase 6b Task 14c: re-injeta regras locais após adoptConfig sobrescrever o blob

        Alert a;
        a.type = AlertType::CONFIG_ADOPTED;
        a.node = mp.from;
        a.arg = reasm.epoch();
        a.atMs = millis();
        gateway.alerts.push(a);
        LOG_INFO("Irrigation GW: adopted config epoch=%u from 0x%08x", reasm.epoch(), mp.from);
    } else {
        LOG_WARN("Irrigation GW: SET_CONFIG blob too large (%u) from 0x%08x", reasm.blobLen(), mp.from);
    }
    reasm.reset();
}

// --- Fase 8a — provisionamento WiFi STA ---
// Glue real somente no ESP32; stubs vazios garantem linkagem no native.
#if defined(ARCH_ESP32)
#include "mesh/wifi/WiFiAPClient.h" // needReconnect (extern bool)
#include <WiFi.h>

void IrrigationModule::portalWifiStatus(IrrigationWeb::WifiStatusCtx &out)
{
    out.enabled = config.network.wifi_enabled;
    out.staUp = WiFi.isConnected();
    if (out.staUp) {
        strncpy(out.connectedSsid, WiFi.SSID().c_str(), sizeof(out.connectedSsid) - 1);
        strncpy(out.ip, WiFi.localIP().toString().c_str(), sizeof(out.ip) - 1);
    }
}

void IrrigationModule::portalWifiStartScan()
{
    WiFi.scanDelete();
    WiFi.scanNetworks(true /*async*/, false /*hidden*/);
}

void IrrigationModule::portalWifiScanResult(IrrigationWeb::WifiScanCtx &out)
{
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) { // -1: ainda em curso → mantém spinner
        out.scanning = true;
        return;
    }
    out.scanning = false;
    if (n < 0) { // WIFI_SCAN_FAILED(-2) ou erro: lista vazia (não trava o spinner)
        out.count = 0;
        return;
    }
    uint8_t cnt = 0;
    for (int16_t i = 0; i < n && cnt < 16; i++) {
        strncpy(out.items[cnt].ssid, WiFi.SSID(i).c_str(), sizeof(out.items[cnt].ssid) - 1);
        out.items[cnt].rssi = (int16_t)WiFi.RSSI(i);
        out.items[cnt].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        cnt++;
    }
    out.count = cnt;
}

bool IrrigationModule::portalWifiConnect(const IrrigationWeb::WifiConnectReq &req)
{
    strncpy(config.network.wifi_ssid, req.ssid, sizeof(config.network.wifi_ssid) - 1);
    strncpy(config.network.wifi_psk, req.psk, sizeof(config.network.wifi_psk) - 1);
    config.network.wifi_enabled = true;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    needReconnect = true;
    return true;
}

void IrrigationModule::portalWifiConnectProgress(IrrigationWeb::WifiConnectCtx &out)
{
    strncpy(out.ssid, config.network.wifi_ssid, sizeof(out.ssid) - 1);
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        out.state = IrrigationWeb::WifiConnectState::Success;
    } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
        out.state = IrrigationWeb::WifiConnectState::Error;
        strncpy(out.error, st == WL_NO_SSID_AVAIL ? "Rede nao encontrada." : "Senha incorreta.",
                sizeof(out.error) - 1);
    } else {
        out.state = IrrigationWeb::WifiConnectState::Connecting;
    }
}

void IrrigationModule::portalWifiForget()
{
    config.network.wifi_ssid[0] = '\0';
    config.network.wifi_psk[0] = '\0';
    config.network.wifi_enabled = false;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    WiFi.disconnect(false, true);
}

void IrrigationModule::portalWifiToggle(bool enabled)
{
    config.network.wifi_enabled = enabled;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    if (enabled) {
        needReconnect = true;
    } else {
        WiFi.disconnect(false, true);
    }
}
#else
void IrrigationModule::portalWifiStatus(IrrigationWeb::WifiStatusCtx &) {}
void IrrigationModule::portalWifiStartScan() {}
void IrrigationModule::portalWifiScanResult(IrrigationWeb::WifiScanCtx &) {}
bool IrrigationModule::portalWifiConnect(const IrrigationWeb::WifiConnectReq &) { return false; }
void IrrigationModule::portalWifiConnectProgress(IrrigationWeb::WifiConnectCtx &) {}
void IrrigationModule::portalWifiForget() {}
void IrrigationModule::portalWifiToggle(bool) {}
#endif

// ---------------------------------------------------------------------------
// Modo Remoto (Task 6): persistência da tabela de associações botoeira→saída.
// ---------------------------------------------------------------------------

bool IrrigationModule::loadRemoteButtons()
{
    size_t n = 0;
    uint8_t buf[6 + RemoteButtonTable::MAX * 27];
    if (!stagedRead(GW_REMOTE_PATH, buf, sizeof(buf), n))
        return false; // arquivo ausente na primeira inicialização — ok, tabela vazia
    return gateway.remoteButtons.deserialize(buf, n);
}

bool IrrigationModule::saveRemoteButtons()
{
    uint8_t buf[6 + RemoteButtonTable::MAX * 27];
    size_t n = gateway.remoteButtons.serialize(buf, sizeof(buf));
    return stagedWrite(GW_REMOTE_TMP, GW_REMOTE_PATH, buf, n);
}

// ---------------------------------------------------------------------------
// Modo Remoto (Task 6 fix): estado autoritativo de zona aberta.
// Fontes:
//   - Zona local (node == selfNode): driver local — valves.isOpen / gpos.isOn.
//   - Zona de grupo:               groupEngine.currentZone + stateOf (motor hidráulico).
//   - Zona remota livre:           StationTelemetryCache — bits valveStates/gpoStates
//                                  populados pelo último HB ou ACK da estação.
// ---------------------------------------------------------------------------

bool IrrigationModule::gwZoneIsOpen(uint8_t zoneId) const
{
    const Zone *z = gateway.zones.byId(zoneId);
    if (!z)
        return false;

    // Zona local: lê o driver diretamente (estado 100% autoritativo).
    if (isLocalTarget(z->node, nodeDB->getNodeNum())) {
        if (z->tipo == 1)
            return gpos.isOn(z->index);
        return valves.isOpen(z->index);
    }

    // Zona de grupo hidráulico: motor é a fonte de verdade.
    const HydraulicGroup *hg = gateway.groups.byZone(zoneId);
    if (hg) {
        return gateway.groupEngine.currentZone(hg->id) == zoneId &&
               gateway.groupEngine.stateOf(hg->id) == HydraulicGroupEngine::State::RUNNING;
    }

    // Zona remota livre: estado reportado pelo nó via HB ou ACK.
    const StationTelemetry *tel = gateway.telemetry.byNode(z->node);
    if (!tel)
        return false;
    if (z->tipo == 1)
        return (tel->gpoStates >> z->index) & 1u;
    return (tel->valveStates >> z->index) & 1u;
}

// ---------------------------------------------------------------------------
// Modo Remoto (Task 7): handler de MSG_REMOTE_LED na estação.
// Aceita somente do gateway vinculado (boundGateway); sem boundGateway, aceita de qualquer nó.
// ---------------------------------------------------------------------------

void IrrigationModule::handleRemoteLed(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &)
{
    // Checagem leve de origem: posse da PSK do canal já garante confiança; isto é defesa extra.
    if (settings.boundGateway != 0 && mp.from != settings.boundGateway)
        return;
    IrrigationProto::RemoteLed rl;
    if (!decodeRemoteLed(mp.decoded.payload.bytes, mp.decoded.payload.size, rl))
        return;
    for (uint8_t s = 0; s < 2; s++)
        _remoteLed[s].onLedState((rl.ledStates >> s) & 1u, millis());
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++)
        _fallback.clear(i);
}

// ---------------------------------------------------------------------------
// Modo Remoto (Task 6 Step 4): handler de MSG_REMOTE_TRIGGER no gateway.
// ---------------------------------------------------------------------------

void IrrigationModule::handleRemoteTrigger(const meshtastic_MeshPacket &mp, const IrrigationProto::Header &h)
{
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation GW: replayed remote-trigger seq %u from 0x%08x", h.seq, mp.from);
        return;
    }
    if (!senderAuthorized(mp.from, h.flags))
        return;
    if (!rateLimiter.allow(millis()))
        return;
    IrrigationProto::RemoteTrigger rt;
    if (!decodeRemoteTrigger(mp.decoded.payload.bytes, mp.decoded.payload.size, rt))
        return;
    gwFireRemote(mp.from, rt.inputIdx);
}

// ---------------------------------------------------------------------------
// Modo Remoto (Task 6 Step 5): resolve associações e toggla saída.
// ---------------------------------------------------------------------------

void IrrigationModule::gwRunCommandOpen(uint8_t zoneId)
{
    const Zone *z = gateway.zones.byId(zoneId);
    if (!z)
        return;
    if (safeMode)
        return;
    if (gateway.interlockEngine.zoneVerdict(z->id).bloqueada)
        return;
    uint16_t dur = z->padraoMin > 0 ? (uint16_t)(z->padraoMin * 60) : DEFAULT_MANUAL_OPEN_S;
    if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60))
        dur = (uint16_t)(z->maxMin * 60);
    if (routeZoneToGroup(z->id, true, dur))
        return;
    const StationEntry *st = gateway.stations.byNode(z->node);
    uint8_t attempts = (st && st->retries > 0) ? st->retries : 3;
    gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts, AuditOrigin::MODO_REMOTO);
}

void IrrigationModule::gwRunCommandClose(uint8_t zoneId)
{
    const Zone *z = gateway.zones.byId(zoneId);
    if (!z)
        return;
    if (routeZoneToGroup(z->id, false, 0))
        return;
    gateway.openGate.release(z->id);
    gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1, AuditOrigin::MODO_REMOTO);
}

void IrrigationModule::gwFireRemote(uint32_t node, uint8_t inputIdx)
{
    const RemoteAssoc *hits[RemoteButtonTable::MAX];
    size_t k = gateway.remoteButtons.findByTrigger(node, inputIdx, hits, RemoteButtonTable::MAX);
    for (size_t i = 0; i < k; i++) {
        const RemoteAssoc *a = hits[i];
        if (!a->enabled)
            continue;
        if (!gateway.zones.byId(a->targetZoneId))
            continue;
        bool on = gwZoneIsOpen(a->targetZoneId);
        uint8_t action = remoteToggleAction(on);
        if (action == 1)
            gwRunCommandOpen(a->targetZoneId);
        else
            gwRunCommandClose(a->targetZoneId);
        gwPushRemoteLed();
    }
}

// ---------------------------------------------------------------------------
// Modo Remoto (Task 6 Step 7): push de LED de feedback change-driven.
// ---------------------------------------------------------------------------

uint8_t IrrigationModule::_ledCacheLookup(uint32_t node) const
{
    for (size_t i = 0; i < _ledCacheCount; i++)
        if (_ledCache[i].node == node)
            return _ledCache[i].states;
    return 0xFF;
}

void IrrigationModule::_ledCacheSet(uint32_t node, uint8_t states)
{
    for (size_t i = 0; i < _ledCacheCount; i++) {
        if (_ledCache[i].node == node) {
            _ledCache[i].states = states;
            return;
        }
    }
    if (_ledCacheCount < LED_CACHE_MAX) {
        _ledCache[_ledCacheCount].node = node;
        _ledCache[_ledCacheCount].states = states;
        _ledCacheCount++;
    }
}

void IrrigationModule::applyLocalRemoteLeds(uint8_t states)
{
    for (uint8_t slot = 0; slot <= 1; slot++) {
        bool on = (states >> slot) & 1u;
        _remoteLed[slot].onLedState(on, millis());
        if (settings.pinsRemoteLed[slot] >= 0) {
#ifndef ARCH_PORTDUINO
            digitalWrite((uint8_t)settings.pinsRemoteLed[slot], on ? HIGH : LOW);
#endif
        }
    }
}

void IrrigationModule::gwPushRemoteLed()
{
    // Acumula, por nó-gatilho, o estado de LED consolidado de TODAS as associações.
    struct NodeLed {
        uint32_t node;
        uint8_t states;
    };
    NodeLed acc[RemoteButtonTable::MAX * RemoteAssoc::MAX_TRIGGERS];
    size_t na = 0;

    for (size_t ai = 0; ai < RemoteButtonTable::MAX; ai++) {
        const RemoteAssoc *a = gateway.remoteButtons.assocAt(ai);
        if (!a)
            break;
        bool on = gwZoneIsOpen(a->targetZoneId);
        for (size_t ti = 0; ti < RemoteAssoc::MAX_TRIGGERS; ti++) {
            const RemoteTriggerRef &t = a->triggers[ti];
            if (t.node == 0 || t.ledSlot > 1)
                continue;
            size_t j = 0;
            for (; j < na; j++)
                if (acc[j].node == t.node)
                    break;
            if (j == na) {
                if (na >= sizeof(acc) / sizeof(acc[0]))
                    continue; // sem espaço no acumulador
                acc[na++] = {t.node, 0};
            }
            if (on)
                acc[j].states |= (uint8_t)(1u << t.ledSlot);
        }
    }

    for (size_t j = 0; j < na; j++) {
        uint8_t prev = _ledCacheLookup(acc[j].node);
        if (prev != 0xFF && prev == acc[j].states)
            continue; // sem mudança
        _ledCacheSet(acc[j].node, acc[j].states);
        if (acc[j].node == nodeDB->getNodeNum()) {
            applyLocalRemoteLeds(acc[j].states);
            continue;
        }
        meshtastic_MeshPacket *p = allocDataPacket();
        p->to = acc[j].node;
        IrrigationProto::RemoteLed rl;
        rl.ledStates = acc[j].states;
        p->decoded.payload.size = (uint16_t)IrrigationProto::encodeRemoteLed(
            p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, rl);
        if (!p->decoded.payload.size) {
            packetPool.release(p);
            continue;
        }
        txPacket(p);
    }
}

// ---------------------------------------------------------------------------
// Modo Remoto (Task 6 Step 8): helpers de config de botoeira por estação/gateway.
// ---------------------------------------------------------------------------

uint8_t IrrigationModule::gwAllocRemoteId() const
{
    for (uint8_t cand = 1; cand <= (uint8_t)RemoteButtonTable::MAX; cand++)
        if (!gateway.remoteButtons.byId(cand))
            return cand;
    return 0;
}

// Recomputa btnMask e ledIdx para um nó-gatilho com base na tabela inteira.
// Limpa bits que não têm mais gatilho registrado (evita entradas órfãs).
void IrrigationModule::gwRecomputeNodeBtnConfig(uint32_t node)
{
    uint8_t btnMask = 0;
    uint8_t ledIdx = 0xFF; // default: todos os slots = 3 (nenhum)

    for (size_t ai = 0; ai < RemoteButtonTable::MAX; ai++) {
        const RemoteAssoc *a = gateway.remoteButtons.assocAt(ai);
        if (!a)
            break;
        for (size_t ti = 0; ti < RemoteAssoc::MAX_TRIGGERS; ti++) {
            const RemoteTriggerRef &t = a->triggers[ti];
            if (t.node != node)
                continue;
            if (t.inputIdx < IrrigationSettings::MAX_DIGITAL_IN) {
                btnMask |= (uint8_t)(1u << t.inputIdx);
                if (t.ledSlot <= 1) {
                    // Limpa os 2 bits do slot de LED para esta entrada e escreve o novo valor.
                    uint8_t shift = (uint8_t)(2u * t.inputIdx);
                    ledIdx &= ~(uint8_t)(0x3u << shift);
                    ledIdx |= (uint8_t)(t.ledSlot << shift);
                }
            }
        }
    }

    if (node == nodeDB->getNodeNum()) {
        // Nó local: grava nos settings do próprio gateway.
        settings.digitalInBtnMask = btnMask;
        settings.digitalInLedIdx = ledIdx;
        saveIrrigationSettings(settings);
    } else {
        // Estação remota: muta blob desejado + bump epoch + re-push SET_CONFIG.
        StationEntry *e = gateway.stations.mutableByNode(node);
        if (!e || e->desiredEpoch == 0)
            return;
        IrrigationSettings cfg;
        if (!migrateIrrigationSettings(e->blob, sizeof(e->blob), cfg))
            return;
        cfg.digitalInBtnMask = btnMask;
        cfg.digitalInLedIdx = ledIdx;
        memcpy(e->blob, &cfg, sizeof(e->blob));
        e->desiredEpoch += 1;
        saveGatewayState();
        const StationTelemetry *tel = gateway.telemetry.byNode(node);
        gwReconcileEpoch(node, tel ? tel->configEpoch : 0);
    }
}

void IrrigationModule::gwPushStationBtnConfig(const RemoteAssoc &a)
{
    // Recomputa para todos os nós-gatilho desta associação.
    for (size_t ti = 0; ti < RemoteAssoc::MAX_TRIGGERS; ti++) {
        uint32_t node = a.triggers[ti].node;
        if (node == 0)
            continue;
        gwRecomputeNodeBtnConfig(node);
    }
}

// ---------------------------------------------------------------------------
// Modo Remoto (Task 6 Step 8): apply-helpers de CRUD.
// ---------------------------------------------------------------------------

bool IrrigationModule::gwApplyRemoteUpsert(const IrrigationWeb::RemoteUpsertReq &u)
{
    if (!IrrigationWeb::validateRemoteTriggers(u))
        return false;
    RemoteAssoc a;
    a.id = u.id ? u.id : gwAllocRemoteId();
    if (a.id == 0)
        return false; // tabela cheia
    a.enabled = u.enabled;
    a.targetZoneId = u.targetZoneId;
    for (size_t i = 0; i < RemoteAssoc::MAX_TRIGGERS; i++)
        a.triggers[i] = u.triggers[i];
    if (!gateway.remoteButtons.upsert(a))
        return false;
    saveRemoteButtons();
    gwPushStationBtnConfig(a);
    gwPushRemoteLed();
    return true;
}

bool IrrigationModule::gwApplyRemoteDelete(uint8_t id)
{
    const RemoteAssoc *a = gateway.remoteButtons.byId(id);
    if (!a)
        return false;
    // Coleta os nós afetados antes de remover.
    uint32_t affectedNodes[RemoteAssoc::MAX_TRIGGERS] = {0};
    for (size_t ti = 0; ti < RemoteAssoc::MAX_TRIGGERS; ti++)
        affectedNodes[ti] = a->triggers[ti].node;
    if (!gateway.remoteButtons.removeById(id))
        return false;
    saveRemoteButtons();
    // Recomputa config das estações afetadas (limpa bits órfãos).
    for (size_t ti = 0; ti < RemoteAssoc::MAX_TRIGGERS; ti++) {
        if (affectedNodes[ti] != 0)
            gwRecomputeNodeBtnConfig(affectedNodes[ti]);
    }
    // Finding 3: atualiza LEDs dos nós cuja associação foi removida.
    gwPushRemoteLed();
    return true;
}

bool IrrigationModule::gwRunRemoteCommand(uint8_t targetZoneId)
{
    if (!gateway.zones.byId(targetZoneId))
        return false;
    bool on = gwZoneIsOpen(targetZoneId);
    if (remoteToggleAction(on) == 1)
        gwRunCommandOpen(targetZoneId);
    else
        gwRunCommandClose(targetZoneId);
    gwPushRemoteLed();
    return true;
}

// ---------------------------------------------------------------------------
// Modo Remoto (Task 8): acessores públicos para os endpoints CI-only.
// Espelham gwBuildMirror / gwApplyMirrorMapping / gwDeleteMirrorMapping.
// ---------------------------------------------------------------------------

size_t IrrigationModule::remoteBuildStatus(char *buf, size_t cap)
{
    bool zoneOpenById[256] = {};
    size_t cnt = gateway.zones.count();
    for (size_t i = 0; i < cnt; i++) {
        const Zone *z = gateway.zones.zoneAt(i);
        if (z)
            zoneOpenById[z->id] = gwZoneIsOpen(z->id);
    }
    return IrrigationWeb::buildRemoteStatus(buf, cap, gateway.remoteButtons, gateway.zones,
                                            zoneOpenById);
}

bool IrrigationModule::remoteApplyUpsert(const char *json, size_t n)
{
    IrrigationWeb::RemoteUpsertReq u;
    if (!IrrigationWeb::parseRemoteUpsert(json, n, u))
        return false;
    return gwApplyRemoteUpsert(u);
}

bool IrrigationModule::remoteApplyDelete(const char *json, size_t n)
{
    uint8_t id = 0;
    if (!IrrigationWeb::parseRemoteDelete(json, n, id))
        return false;
    return gwApplyRemoteDelete(id);
}

bool IrrigationModule::remoteRunCommand(const char *json, size_t n)
{
    uint8_t targetZoneId = 0;
    if (!IrrigationWeb::parseRemoteCommand(json, n, targetZoneId))
        return false;
    return gwRunRemoteCommand(targetZoneId);
}
