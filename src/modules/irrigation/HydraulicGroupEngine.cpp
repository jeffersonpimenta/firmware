#include "HydraulicGroupEngine.h"

void HydraulicGroupEngine::reset() { *this = HydraulicGroupEngine{}; }

HydraulicGroupEngine::GroupRt &HydraulicGroupEngine::rtOf(uint8_t groupId) { return rt[groupId - 1]; }

HydraulicGroupEngine::ZoneRt *HydraulicGroupEngine::findZone(GroupRt &g, uint8_t zoneId)
{
    for (auto &z : g.zones)
        if (z.zoneId == zoneId)
            return &z;
    return nullptr;
}

HydraulicGroupEngine::ZoneRt *HydraulicGroupEngine::ensureZone(GroupRt &g, uint8_t zoneId)
{
    if (ZoneRt *z = findZone(g, zoneId))
        return z;
    for (auto &z : g.zones)
        if (z.zoneId == 0) {
            z = ZoneRt{};
            z.zoneId = zoneId;
            return &z;
        }
    return nullptr;
}

void HydraulicGroupEngine::pushAlert(uint8_t groupId, uint8_t code, uint8_t zoneId)
{
    if (aCount >= 16) { aHead = (aHead + 1) % 16; aCount--; } // ring cheio: descarta o mais antigo
    alertRing[aTail] = {groupId, code, zoneId};
    aTail = (aTail + 1) % 16;
    aCount++;
}

bool HydraulicGroupEngine::takeAlert(GroupAlert &out)
{
    if (aCount == 0)
        return false;
    out = alertRing[aHead];
    aHead = (aHead + 1) % 16;
    aCount--;
    return true;
}

bool HydraulicGroupEngine::resolve(const ZoneTable &zones, uint8_t zoneId, GroupEmit &e) const
{
    const Zone *z = zones.byId(zoneId);
    if (!z)
        return false;
    e.node = z->node;
    e.index = z->index;
    e.tipo = z->tipo;
    e.zoneId = zoneId;
    return true;
}

uint16_t HydraulicGroupEngine::pumpDur(uint16_t zoneDurS) const
{
    uint32_t d = (uint32_t)zoneDurS + PUMP_MARGIN_S;
    return (uint16_t)(d > PUMP_CEILING_S ? PUMP_CEILING_S : d);
}

size_t HydraulicGroupEngine::confirmedCount(const GroupRt &g) const
{
    size_t c = 0;
    for (const auto &z : g.zones)
        if (z.zoneId && z.confirmed)
            c++;
    return c;
}

void HydraulicGroupEngine::setDesired(uint8_t groupId, uint8_t zoneId, bool open, uint16_t durationS)
{
    if (groupId == 0 || groupId > MAX_GROUPS)
        return;
    GroupRt &g = rtOf(groupId);
    ZoneRt *z = ensureZone(g, zoneId);
    if (!z)
        return;
    z->wanted = open;
    if (open)
        z->wantDurS = durationS;
}

// INVARIANTE: o glue deve chamar noteSent() no mesmo ciclo do emit (antes de qualquer
// onAck do próximo pump de mensagens). Se o ACK chegasse antes de noteSent, o pend.seq
// ficaria 0 e o onAck não casaria. O glue síncrono do gwTick garante essa ordem.
void HydraulicGroupEngine::noteSent(uint32_t node, uint8_t zoneId, uint8_t action, uint32_t seq)
{
    for (auto &g : rt)
        if (g.pend.inUse && g.pend.node == node && g.pend.zoneId == zoneId && g.pend.action == action) {
            g.pend.seq = seq;
            return;
        }
}

void HydraulicGroupEngine::onAck(uint32_t node, uint32_t ackedSeq)
{
    for (size_t i = 0; i < MAX_GROUPS; i++) {
        GroupRt &g = rt[i];
        if (!g.pend.inUse || g.pend.node != node || g.pend.seq != ackedSeq)
            continue;
        uint8_t za = g.pend.zoneId, ac = g.pend.action;
        g.pend.inUse = false;
        // Marca confirmação da zona (a bomba não tem ZoneRt: findZone devolve nullptr e o
        // avanço de estado vem só do switch — PUMP_WAIT_ACK/PUMP_OFF_WAIT).
        if (ZoneRt *z = findZone(g, za)) {
            z->confirmed = (ac == 1);
            if (ac == 0) { z->wanted = false; z->localExpiresMs = 0; } // fechou: sai do confirmado
        }
        // Avança o estado que aguardava este ACK. O timer da espera seguinte (START_WAIT,
        // X_OVERLAP, DRAIN) já foi ancorado em waitStartMs no instante do emit; NÃO resetar aqui.
        switch (g.state) {
        case State::OPENING:
            g.state = State::START_WAIT;
            break;
        case State::PUMP_WAIT_ACK:
            g.pump = true;
            g.state = State::RUNNING;
            break;
        case State::X_OPEN_WAIT:
            g.state = State::X_OVERLAP;
            break;
        case State::X_CLOSE_WAIT:
            g.state = State::RUNNING;
            g.curZone = g.nextZone;
            g.nextZone = 0;
            break;
        case State::PUMP_OFF_WAIT:
            g.pump = false;
            g.state = State::DRAIN;
            break;
        case State::CLOSE_LAST_WAIT:
            g.state = State::IDLE;
            g.curZone = 0;
            break;
        default:
            break;
        }
        return;
    }
}

void HydraulicGroupEngine::onCmdFailed(uint32_t, uint8_t, uint8_t) { /* Task 5/6 */ }
void HydraulicGroupEngine::observeActual(uint8_t, uint8_t, bool) { /* Task 7 */ }

HydraulicGroupEngine::State HydraulicGroupEngine::stateOf(uint8_t groupId) const
{
    if (groupId == 0 || groupId > MAX_GROUPS)
        return State::IDLE;
    return rt[groupId - 1].state;
}
bool HydraulicGroupEngine::pumpOn(uint8_t groupId) const
{
    if (groupId == 0 || groupId > MAX_GROUPS)
        return false;
    return rt[groupId - 1].pump;
}

uint8_t HydraulicGroupEngine::pruneStarts(GroupRt &, uint32_t) const { return 0; } // Task 6

size_t HydraulicGroupEngine::tick(const HydraulicGroupTable &tbl, const ZoneTable &zones, uint32_t nowMs,
                                  GroupEmit *out, size_t cap)
{
    size_t emitted = 0;
    for (size_t i = 0; i < MAX_GROUPS && emitted < cap; i++) {
        GroupRt &g = rt[i];
        uint8_t groupId = (uint8_t)(i + 1);
        const HydraulicGroup *cfg = tbl.byId(groupId);
        if (!cfg)
            continue;
        if (g.pend.inUse)
            continue; // aguardando ACK: 1 comando em voo por grupo

        auto firstWantedUnconfirmed = [&]() -> ZoneRt * {
            for (auto &z : g.zones)
                if (z.zoneId && z.wanted && !z.confirmed)
                    return &z;
            return nullptr;
        };
        auto anyWanted = [&]() -> bool {
            for (auto &z : g.zones)
                if (z.zoneId && z.wanted)
                    return true;
            return false;
        };
        auto confirmedNotWanted = [&]() -> ZoneRt * {
            for (auto &z : g.zones)
                if (z.zoneId && z.confirmed && !z.wanted)
                    return &z;
            return nullptr;
        };

        GroupEmit e;
        switch (g.state) {
        case State::IDLE: {
            ZoneRt *w = firstWantedUnconfirmed();
            if (!w)
                break;
            if (!resolve(zones, w->zoneId, e))
                break;
            e.action = 1;
            e.durationS = w->wantDurS;
            g.pend = {true, e.node, 0, w->zoneId, 1};
            g.curZone = w->zoneId;
            g.waitStartMs = nowMs; // âncora do START_WAIT (partida_apos_abrir_s)
            g.state = State::OPENING;
            out[emitted++] = e;
            break;
        }
        case State::START_WAIT: {
            // minOpen>1: abre min_abertas_com_bomba válvulas ANTES da bomba. Enquanto faltarem
            // válvulas confirmadas, emite o próximo open e re-ancora o timer (emit-anchored).
            if (confirmedCount(g) < (size_t)cfg->minOpen) {
                ZoneRt *w = firstWantedUnconfirmed();
                if (!w) // nada mais a abrir: segue com o que tem (evita travar)
                    ;
                else {
                    if (!resolve(zones, w->zoneId, e))
                        break;
                    e.action = 1;
                    e.durationS = w->wantDurS;
                    g.pend = {true, e.node, 0, w->zoneId, 1};
                    g.curZone = w->zoneId;
                    g.waitStartMs = nowMs; // re-ancora START_WAIT no emit do próximo open
                    g.state = State::OPENING;
                    out[emitted++] = e;
                    break;
                }
            }
            // Timer ancorado no INSTANTE DO EMIT do open (não no ACK): mede partida_apos_abrir_s
            // desde o envio do comando. Para ACK local rápido é equivalente; NÃO re-ancorar no onAck.
            if (nowMs - g.waitStartMs < (uint32_t)cfg->startAfterOpenS * 1000)
                break;
            if (cfg->bombaZoneId == 0) { // grupo sem bomba: vai direto p/ RUNNING
                g.state = State::RUNNING;
                break;
            }
            if (!resolve(zones, cfg->bombaZoneId, e))
                break;
            e.action = 1;
            ZoneRt *cur = findZone(g, g.curZone);
            e.durationS = pumpDur((cur && cur->wantDurS) ? cur->wantDurS : 600u);
            g.pend = {true, e.node, 0, cfg->bombaZoneId, 1};
            g.state = State::PUMP_WAIT_ACK;
            pushAlert(groupId, GA_PUMP_ON, cfg->bombaZoneId);
            out[emitted++] = e;
            break;
        }
        case State::RUNNING: {
            ZoneRt *w = firstWantedUnconfirmed();       // nova zona a abrir (transição)
            if (w) {
                if (cfg->transicao == 1) { // fechar_antes_de_abrir
                    ZoneRt *old = confirmedNotWanted();
                    if (old) { // fecha a antiga PRIMEIRO (sem overlap)
                        if (!resolve(zones, old->zoneId, e))
                            break;
                        e.action = 0;
                        g.pend = {true, e.node, 0, old->zoneId, 0};
                        g.nextZone = w->zoneId;
                        g.state = State::X_CLOSE_WAIT; // onAck volta a RUNNING; próximo tick abre a nova
                        out[emitted++] = e;
                        break;
                    }
                    // sem antiga a fechar: cai no fluxo de abrir (abaixo)
                }
                if (!resolve(zones, w->zoneId, e))
                    break;
                e.action = 1;
                e.durationS = w->wantDurS;
                g.pend = {true, e.node, 0, w->zoneId, 1};
                g.nextZone = w->zoneId;
                g.waitStartMs = nowMs;        // âncora do X_OVERLAP (sobreposicao_s)
                g.state = State::X_OPEN_WAIT; // abrir_antes_de_fechar
                out[emitted++] = e;
                break;
            }
            if (!anyWanted()) {                          // fim: desliga bomba
                if (cfg->bombaZoneId != 0 && g.pump) {
                    if (!resolve(zones, cfg->bombaZoneId, e))
                        break;
                    e.action = 0;
                    g.pend = {true, e.node, 0, cfg->bombaZoneId, 0};
                    g.waitStartMs = nowMs;   // âncora do DRAIN (parar_antes_de_fechar_s)
                    g.state = State::PUMP_OFF_WAIT;
                    pushAlert(groupId, GA_PUMP_OFF, cfg->bombaZoneId);
                    out[emitted++] = e;
                } else {
                    g.waitStartMs = nowMs;
                    g.state = State::DRAIN;
                }
                break;
            }
            break;
        }
        case State::X_OVERLAP: {
            if (nowMs - g.waitStartMs < (uint32_t)cfg->overlapS * 1000)
                break;
            ZoneRt *old = confirmedNotWanted();
            if (!old) { // nada a fechar: volta a RUNNING
                g.state = State::RUNNING;
                g.curZone = g.nextZone;
                g.nextZone = 0;
                break;
            }
            if (!resolve(zones, old->zoneId, e))
                break;
            e.action = 0;
            g.pend = {true, e.node, 0, old->zoneId, 0};
            g.state = State::X_CLOSE_WAIT;
            out[emitted++] = e;
            break;
        }
        case State::DRAIN: {
            if (nowMs - g.waitStartMs < (uint32_t)cfg->stopBeforeCloseS * 1000)
                break;
            ZoneRt *last = confirmedNotWanted();
            if (!last) {
                g.state = State::IDLE;
                g.curZone = 0;
                break;
            }
            if (!resolve(zones, last->zoneId, e))
                break;
            e.action = 0;
            g.pend = {true, e.node, 0, last->zoneId, 0};
            g.state = State::CLOSE_LAST_WAIT;
            out[emitted++] = e;
            break;
        }
        default:
            break;
        }
    }
    return emitted;
}
