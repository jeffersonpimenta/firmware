#include "modules/irrigation/IrrigationModule.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "Throttle.h"
#include "configuration.h"
#include "main.h"
#include "mesh/Channels.h"
#include <string.h>

// Default manual open duration when the user double-presses the station button (spec §5.2).
static constexpr uint32_t DEFAULT_MANUAL_OPEN_S = 20 * 60; // 20 min

static const char *ALLOWLIST_PATH = "/prefs/irrigation-allow.dat";
static const char *ALLOWLIST_TMP = "/prefs/irrigation-allow.tmp";

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
    if ((IrrigationRole)settings.role == IrrigationRole::GATEWAY)
        loadAllowlist();
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
    case MSG_HEARTBEAT:
        // Lado gateway chega na Fase 4; por ora só loga
        LOG_DEBUG("Irrigation: type %d from 0x%08x (ignored, role=%d)", h.type, mp.from, settings.role);
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
        gatewayPairing.tick(millis());
        refreshLedMode();
        return gatewayPairing.windowOpen() ? 1000 : 60 * 1000; // janela aberta: pisca em 1 s; idle: 60 s
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

