#include "modules/irrigation/IrrigationModule.h"
#include "FSCommon.h"
#include "modules/irrigation/IrrigationWebApi.h"
#include "modules/irrigation/PortalApi.h"
#include "MeshService.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "Throttle.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "main.h"
#include "mesh/Channels.h"
#include <string.h>

// Default manual open duration when the user double-presses the station button (spec §5.2).
static constexpr uint32_t DEFAULT_MANUAL_OPEN_S = 20 * 60; // 20 min

static const char *ALLOWLIST_PATH = "/prefs/irrigation-allow.dat";
static const char *ALLOWLIST_TMP = "/prefs/irrigation-allow.tmp";

// Persistência do estado gateway (Task 6, decisão §5) — staged-write em 4 arquivos.
static const char *GW_STATIONS_PATH = "/prefs/irrigation-stations.dat";
static const char *GW_STATIONS_TMP = "/prefs/irrigation-stations.tmp";
static const char *GW_ZONES_PATH = "/prefs/irrigation-zones.dat";
static const char *GW_ZONES_TMP = "/prefs/irrigation-zones.tmp";
static const char *GW_PROGRAMS_PATH = "/prefs/irrigation-programs.dat";
static const char *GW_PROGRAMS_TMP = "/prefs/irrigation-programs.tmp";
static const char *GW_MIRROR_PATH = "/prefs/irrigation-mirror.dat";
static const char *GW_MIRROR_TMP = "/prefs/irrigation-mirror.tmp";

// Cooldown de reconciliação de epoch por nó (30 s)
static constexpr uint32_t EPOCH_COOLDOWN_MS = 30000;

IrrigationModule *irrigationModule;

using namespace IrrigationProto;

static IrrigationSettings loadIrrigationSettingsOrDefault()
{
    IrrigationSettings s;
    loadIrrigationSettings(s);
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
      rateLimiter(settings.cmdRatePerMin)
{
    IrrigationSettings probe;
    safeMode = !loadIrrigationSettings(probe); // sem config persistida = modo seguro (spec §5.5)
    driver.configure(settings);
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
    }
    LOG_INFO("IrrigationModule role=%d valves=%d gateway=0x%08x epoch=%u safe=%d", settings.role, settings.numValves,
             settings.boundGateway, settings.configEpoch, (int)safeMode);
    // Spin up the GPIO button/LED thread only when at least one pin is wired (spec §8.6/§8.7).
    // The class is defined later in this translation unit; new'd here so it owns its own lifetime.
    if (settings.pinBtn >= 0 || settings.pinLed >= 0)
        new IrrigationUiThread(this, settings.pinBtn, settings.pinLed);
}

bool IrrigationModule::wantPacket(const meshtastic_MeshPacket *p)
{
    if ((IrrigationRole)settings.role == IrrigationRole::REPETIDOR)
        return false; // repetidor só retransmite (spec §3.1)
    return p->decoded.portnum == ourPortNum;
}

bool IrrigationModule::senderAuthorized(uint32_t from) const
{
    // Fase 3 (pareamento) elimina o modo aberto: hoje, sem vínculo gravado,
    // qualquer nó do canal comanda (posse da PSK = autoridade, spec §4.2).
    return settings.boundGateway == 0 || from == settings.boundGateway;
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
    if (settings.boundGateway != 0 && mp.from == settings.boundGateway)
        lastGatewayRxMs = millis();

    switch (h.type) {
    case MSG_CMD_VALVULA:
        handleCmdValvula(mp, h);
        break;
    case MSG_SET_CONFIG:
        // Task 6, decisão §3: role split — GATEWAY recebe SET_CONFIG como resposta de GET_CONFIG.
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            handleGwSetConfig(mp, h);
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
    case MSG_ACK:
        // Task 6, decisão §3: gateway trata ACKs das estações.
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            handleGwAck(mp, h);
        else
            LOG_DEBUG("Irrigation: ACK from 0x%08x ignored (role=%d)", mp.from, settings.role);
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
    default:
        LOG_DEBUG("Irrigation: unhandled type %d from 0x%08x", h.type, mp.from);
        break;
    }
    return ProcessMessage::STOP;
}

void IrrigationModule::handleCmdValvula(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from)) {
        LOG_WARN("Irrigation: unauthorized cmd from 0x%08x", mp.from);
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
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
        return;
    }

    CmdValvula cmd;
    if (!decodeCmdValvula(mp.decoded.payload.bytes, mp.decoded.payload.size, cmd)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        return;
    }

    if (safeMode && cmd.action == 1) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_SAFE_MODE);
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
    sendAck(mp.from, h.seq, reason == REASON_NONE ? ACK_OK : ACK_NACK, reason);
}

void IrrigationModule::sendAck(uint32_t to, uint32_t ackedSeq, uint8_t status, uint8_t reason)
{
    Ack ack = {};
    ack.ackedSeq = ackedSeq;
    ack.status = status;
    ack.reason = reason;
    ack.valveStates = valves.stateBitmap();
    ack.vbatCentiV = batteryCentiV();
    ack.configEpoch = settings.configEpoch;

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = to;
    p->decoded.payload.size = encodeAck(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, ack);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
}

void IrrigationModule::sendHeartbeat()
{
    Heartbeat hb = {};
    hb.valveStates = valves.stateBitmap();
    hb.vbatCentiV = batteryCentiV();
    hb.configEpoch = settings.configEpoch;
    hb.flags = safeMode ? HB_FLAG_SAFE_MODE : 0;

    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = settings.boundGateway ? settings.boundGateway : NODENUM_BROADCAST;
    p->decoded.payload.size = encodeHeartbeat(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, hb);
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    LOG_DEBUG("Irrigation heartbeat sent, valves=0x%x vbat=%u cV", hb.valveStates, hb.vbatCentiV);
}

uint16_t IrrigationModule::batteryCentiV() const
{
    if (powerStatus && powerStatus->getHasBattery())
        return powerStatus->getBatteryVoltageMv() / 10;
    return 0;
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
    settings = merged;
    if (settings.numValves == 0)
        LOG_WARN("Irrigation: config sets zero valves");
    driver.configure(settings);
    valves.setNumValves(settings.numValves);
    rateLimiter = RateLimiter(settings.cmdRatePerMin);
}

void IrrigationModule::handleSetConfig(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_UNAUTHORIZED);
        return;
    }
    if (!seqTable.checkAndUpdate(mp.from, h.seq)) {
        LOG_WARN("Irrigation: replayed config seq %u from 0x%08x", h.seq, mp.from);
        return;
    }
    if (!rateLimiter.allow(millis())) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_RATE_LIMIT);
        return;
    }

    SetConfig sc;
    if (!decodeSetConfig(mp.decoded.payload.bytes, mp.decoded.payload.size, sc)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        return;
    }

    auto r = reasm.add(mp.from, sc.epoch, sc.crc, sc.totalLen, sc.fragIndex, sc.fragCount, sc.frag, sc.fragLen, millis());
    switch (r) {
    case FragmentReassembler::Add::ACCEPTED:
        return; // fragmento intermediário: sem ACK (ACK = commit, §5.5)
    case FragmentReassembler::Add::TOO_BIG:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_CONFIG_TOO_BIG);
        return;
    case FragmentReassembler::Add::INVALID:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_FRAG_INVALID);
        return;
    case FragmentReassembler::Add::BAD_CRC:
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_CRC);
        return;
    case FragmentReassembler::Add::COMPLETE:
        break;
    }

    IrrigationSettings fresh;
    if (!migrateIrrigationSettings(reasm.blob(), reasm.blobLen(), fresh)) {
        reasm.reset();
        sendAck(mp.from, h.seq, ACK_NACK, REASON_BAD_PAYLOAD);
        return;
    }
    uint32_t newEpoch = reasm.epoch();
    reasm.reset();

    IrrigationSettings merged = mergeRemoteConfig(fresh, newEpoch);
    if (!saveIrrigationSettings(merged)) {
        sendAck(mp.from, h.seq, ACK_NACK, REASON_COMMIT_FAIL);
        return;
    }
    activateSettings(merged);
    safeMode = false;
    LOG_INFO("Irrigation: config applied epoch=%u from 0x%08x", newEpoch, mp.from);
    sendAck(mp.from, h.seq, ACK_OK, REASON_NONE);
}

void IrrigationModule::handleGetConfig(const meshtastic_MeshPacket &mp, const Header &h)
{
    if (!senderAuthorized(mp.from)) {
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
        service->sendToMesh(p, RX_SRC_LOCAL, false);
    }
}

int32_t IrrigationModule::runOnce()
{
    // Fail-safe tick roda em TODOS os papéis: num nó mal-configurado nunca deve
    // sobrar válvula aberta sem timer sendo decrementado.
    valves.tick(millis());
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
            service->sendToMesh(p, RX_SRC_LOCAL, false);
        else
            packetPool.release(p);
    }

    if ((IrrigationRole)settings.role != IrrigationRole::ESTACAO) {
        if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
            gwTick(); // Task 6, decisão §2: loop principal do gateway 1×/s
        refreshLedMode();
        return 1000; // Task 6: cadência de 1 s (substitui 60 s/1 s condicional anterior)
    }
    uint16_t vbat = batteryCentiV();
    valves.setBatteryLockout(vbat != 0 && vbat < settings.vbatMinAbrirCentiV);

    if (bootHeartbeatPending && millis() > 10000) {
        bootHeartbeatPending = false;
        lastHeartbeatMs = millis();
        sendHeartbeat();
    }

    if (!Throttle::isWithinTimespanMs(lastHeartbeatMs, (uint32_t)settings.hbMinutes * 60 * 1000)) {
        lastHeartbeatMs = millis();
        sendHeartbeat();
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
        LOG_INFO("Irrigation: announce from 0x%08x (%.*s) ignored, window closed", mp.from, (int)pa.nameLen, pa.name);
        return;
    }
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
    service->sendToMesh(p, RX_SRC_LOCAL, false);
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
    service->sendToMesh(p, RX_SRC_LOCAL, false);
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
    if (role != IrrigationRole::ESTACAO)
        return;
    // Paired station gestures.
    switch (ev) {
    case Ev::SHORT:
        LOG_INFO("Irrigation: portal request (Phase 5 stub)");
        break;
    case Ev::DOUBLE:
        if (valves.isOpen(0)) {
            valves.close(0);
            sendEvento(EV_MANUAL_CLOSE);
        } else if (valves.open(0, DEFAULT_MANUAL_OPEN_S, settings.maxOpenConfigS, millis()) ==
                   ValveController::Result::OK) {
            sendEvento(EV_MANUAL_OPEN, DEFAULT_MANUAL_OPEN_S);
        }
        break;
    case Ev::LONG_3S:
        if (valves.open(0, 10, settings.maxOpenConfigS, millis()) == ValveController::Result::OK)
            sendEvento(EV_TEST_PULSE);
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
// Gateway state persistence (Task 6, decisão §5) — staged-write, 4 arquivos.
// Helper macro para não repetir o padrão 4×.
// ---------------------------------------------------------------------------

// Staged-write genérico: serializa com fn, grava em tmp, rename.
// Tamanho máximo dos buffers:
//   stations: MAGIC(4)+ver(1)+count(1)+16*87 = 1398 bytes → 1400
//   zones:    MAGIC(4)+ver(1)+count(1)+24*32 = 774  bytes → 800
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
        uint8_t buf[6 + StationRegistry::MAX * 87];
        if (stagedRead(GW_STATIONS_PATH, buf, sizeof(buf), n))
            ok &= gateway.stations.deserialize(buf, n);
    }
    // Zones
    {
        uint8_t buf[6 + ZoneTable::MAX * 32];
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
        uint8_t buf[6 + StationRegistry::MAX * 87];
        size_t n = gateway.stations.serialize(buf, sizeof(buf));
        ok &= stagedWrite(GW_STATIONS_TMP, GW_STATIONS_PATH, buf, n);
    }
    // Zones
    {
        uint8_t buf[6 + ZoneTable::MAX * 32];
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
    return ok;
}

// ---------------------------------------------------------------------------
// Gateway engine — handlers e tick (Task 6, decisões 1–6)
// ---------------------------------------------------------------------------

// Decisão §1: envia CmdValvula ou CmdGpo e registra no tracker.
void IrrigationModule::gwSendValveCmd(uint32_t node, uint8_t index, uint8_t tipo, uint8_t action, uint16_t durationS,
                                      uint8_t zoneId, uint8_t attempts)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = node;
    if (tipo == 1) {
        CmdGpo cmd = {};
        cmd.gpoId = index;
        cmd.action = action;
        cmd.durationS = durationS;
        p->decoded.payload.size = (uint16_t)encodeCmdGpo(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, cmd);
    } else {
        CmdValvula cmd = {};
        cmd.valveId = index;
        cmd.action = action;
        cmd.durationS = durationS;
        p->decoded.payload.size = (uint16_t)encodeCmdValvula(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), ++txSeq, cmd);
    }
    if (!p->decoded.payload.size) {
        packetPool.release(p);
        return;
    }
    service->sendToMesh(p, RX_SRC_LOCAL, false);
    gateway.tracker.track(txSeq, node, zoneId, action, durationS, attempts, millis());
    LOG_DEBUG("Irrigation GW: sent cmd zone=%u node=0x%08x action=%u dur=%u", zoneId, node, action, durationS);
}

// Fonte única de hora local do gateway (mesma que o scheduler consome no gwTick).
bool IrrigationModule::computeLocalSecs(uint32_t &out) const
{
    uint32_t epochLocal = getValidTime(RTCQualityDevice, true);
    out = epochLocal;
    return epochLocal != 0;
}

// --- Serviço do painel web (gateway). Ponte entre a cola HTTP (Task 10) e o estado do gateway. ---
bool IrrigationModule::gwIsGateway() const
{
    return settings.role == (uint8_t)IrrigationRole::GATEWAY;
}

bool IrrigationModule::gwHasRtc() const
{
    uint32_t s = 0;
    return computeLocalSecs(s);
}
uint32_t IrrigationModule::gwLocalSecs() const
{
    uint32_t s = 0;
    computeLocalSecs(s);
    return s;
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
        lastAckAllMs = millis();
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
        uint16_t dur = c.durationS;
        if (z->maxMin > 0 && dur > (uint16_t)(z->maxMin * 60))
            dur = (uint16_t)(z->maxMin * 60);
        gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, z->id, attempts);
        return true;
    }
    if (c.kind == K::CLOSE) {
        gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
        return true;
    }
    return false;
}

// --- Serviço do portal de campo (Fase 5b). Role-agnóstico. ---
void IrrigationModule::portalFillNodeState(IrrigationWeb::NodeStateCtx &out) const
{
    out.role = settings.role;
    out.name = owner.short_name; // extern meshtastic_User& (mesmo uso de handlePairAnnounce)
    out.boundGateway = settings.boundGateway;
    out.configEpoch = settings.configEpoch;
    out.safeMode = safeMode;
    out.numValves = settings.numValves;
    out.valveStates = valves.stateBitmap();
    out.gpoStates = 0;
    out.vbatCentiV = batteryCentiV();
    out.vpanelCentiV = 0;
    out.flags = safeMode ? HB_FLAG_SAFE_MODE : 0;
    out.apSecondsLeft = portal.secondsLeft(millis());
}

bool IrrigationModule::portalPulse(const IrrigationWeb::PortalPulseReq &p)
{
    // Teste de pulso local: abre a válvula com fechamento automático pelo timer fail-safe.
    if (valves.open(p.valveId, p.durationS, settings.maxOpenConfigS, millis()) != ValveController::Result::OK)
        return false;
    sendEvento(EV_TEST_PULSE);
    return true;
}

// Decisão §2: loop principal do gateway — scheduler, espelho, retries, silêncio.
void IrrigationModule::gwTick()
{
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
            if (a.type == SchedAction::Type::OPEN) {
                // Bypass: se o espelho é dono desta zona, ele manda — suprime o OPEN.
                if (mirrorOwnsZoneOutput(gateway.mirror, z->fonteInput)) {
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
                gwSendValveCmd(z->node, z->index, z->tipo, 1, dur, a.zoneId, attempts);
            } else { // CLOSE
                // Mesma proteção: não feche o que o espelho mantém aberto (carryover F4 #1).
                if (mirrorOwnsZoneOutput(gateway.mirror, z->fonteInput)) {
                    LOG_DEBUG("Irrigation GW: scheduler CLOSE zone=%u suprimido (espelho dono)", a.zoneId);
                    continue;
                }
                gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, a.zoneId, 1);
            }
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
        if (ma.t == MirrorMode::Action::T::OPEN) {
            // bypass total: sem clamp de maxMin (estação clampa no teto compilado)
            const StationEntry *stEntry = gateway.stations.byNode(z->node);
            uint8_t attempts = (stEntry && stEntry->retries > 0) ? stEntry->retries : 3;
            gwSendValveCmd(z->node, z->index, z->tipo, 1, MirrorMode::OPEN_S, z->id, attempts);
        } else {
            gwSendValveCmd(z->node, z->index, z->tipo, 0, 0, z->id, 1);
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
                service->sendToMesh(p, RX_SRC_LOCAL, false);
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
        const uint8_t *blob = entry->blob;
        uint16_t totalLen = sizeof(entry->blob);
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
            service->sendToMesh(p, RX_SRC_LOCAL, false);
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
        service->sendToMesh(p, RX_SRC_LOCAL, false);
        LOG_INFO("Irrigation GW: GET_CONFIG from node=0x%08x (remote epoch=%u > desired=%u)", node, remoteEpoch,
                 entry->desiredEpoch);
    }
    // remoteEpoch == desiredEpoch: em sincronia, nada a fazer.
}

// Decisão §3: MSG_ACK recebido pelo gateway.
void IrrigationModule::handleGwAck(const meshtastic_MeshPacket &mp, const Header &h)
{
    Ack ack;
    if (!decodeAck(mp.decoded.payload.bytes, mp.decoded.payload.size, ack)) {
        LOG_WARN("Irrigation GW: bad ACK payload from 0x%08x", mp.from);
        return;
    }

    // Task 6, decisão §6: NACK é resposta definitiva — remove pendência e alerta.
    if (ack.status != ACK_OK) {
        // onAck remove a pendência independente do status.
        if (gateway.tracker.onAck(mp.from, ack.ackedSeq)) {
            LOG_WARN("Irrigation GW: NACK from 0x%08x seq=%u reason=%u", mp.from, ack.ackedSeq, ack.reason);
            // Determina zoneId a partir do ackedSeq — o tracker já removeu o slot,
            // então logamos com zoneId=0 (informação de alerta é best-effort aqui;
            // o diagnóstico detalhado é Fase 6).
            Alert a;
            a.type = AlertType::CMD_FAIL;
            a.node = mp.from;
            a.arg = ack.reason; // reason como arg conforme decisão §6
            a.atMs = millis();
            gateway.alerts.push(a);
        }
    } else {
        gateway.tracker.onAck(mp.from, ack.ackedSeq);
    }

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
    tel.rebootCount = hb.rebootCount;
    tel.flags = hb.flags;
    tel.configEpoch = hb.configEpoch;
    tel.atMs = millis();
    gateway.telemetry.update(tel);
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

    // Blob completo: adoptar como config da estação.
    if (reasm.blobLen() <= sizeof(StationEntry::blob)) {
        gateway.stations.adoptConfig(mp.from, reasm.blob(), reasm.epoch());
        saveGatewayState(); // persiste após mutação (decisão §5)

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

