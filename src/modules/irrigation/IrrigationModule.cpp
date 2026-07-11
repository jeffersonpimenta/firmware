#include "modules/irrigation/IrrigationModule.h"
#include "MeshService.h"
#include "MeshTypes.h"
#include "PowerStatus.h"
#include "Throttle.h"
#include "configuration.h"
#include "main.h"
#include <string.h>

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

IrrigationModule::IrrigationModule()
    : SinglePortModule("irrigation", meshtastic_PortNum_PRIVATE_APP), concurrency::OSThread("Irrigation"),
      settings(loadIrrigationSettingsOrDefault()), valves(driver, settings.numValves),
      rateLimiter(settings.cmdRatePerMin)
{
    // settings already loaded by loadIrrigationSettingsOrDefault() in member init list.
    driver.configure(settings);
    // Boot sempre em estado fechado (spec §5.5): solenoides latching podem ter
    // ficado abertos num reset com válvula acionada. forceCloseAll() pulsa
    // incondicionalmente, sem depender do bit open interno (que está falso
    // em todos os slots recém-criados).
    valves.forceCloseAll();
    LOG_INFO("IrrigationModule role=%d valves=%d gateway=0x%08x", settings.role, settings.numValves, settings.boundGateway);
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

    switch (h.type) {
    case MSG_CMD_VALVULA:
        handleCmdValvula(mp, h);
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
    ack.configEpoch = 0; // epoch chega na Fase 2

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
    hb.configEpoch = 0;

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

int32_t IrrigationModule::runOnce()
{
    // Fail-safe tick roda em TODOS os papéis: num nó mal-configurado nunca deve
    // sobrar válvula aberta sem timer sendo decrementado.
    valves.tick(millis());

    if ((IrrigationRole)settings.role != IrrigationRole::ESTACAO)
        return 60 * 1000; // gateway/repetidor/serviço: nada periódico nesta fase
    uint16_t vbat = batteryCentiV();
    valves.setBatteryLockout(vbat != 0 && vbat < settings.vbatMinAbrirCentiV);

    if (!Throttle::isWithinTimespanMs(lastHeartbeatMs, (uint32_t)settings.hbMinutes * 60 * 1000)) {
        lastHeartbeatMs = millis();
        sendHeartbeat();
    }
    return 1000; // tick de 1 s mantém o fail-safe responsivo
}
