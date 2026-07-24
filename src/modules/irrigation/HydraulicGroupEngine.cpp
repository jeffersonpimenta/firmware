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
            g.reachedRunning = true; // RUNNING estabelecido com bomba on: habilita reconcile (§8.13)
            break;
        case State::X_OPEN_WAIT:
            g.state = State::X_OVERLAP;
            break;
        case State::X_CLOSE_WAIT:
            g.state = State::RUNNING;
            g.curZone = g.nextZone;
            g.nextZone = 0;
            g.closeFailStreak = 0; // fechamento bem-sucedido: zera streak
            g.pumpNeedsRenew = true; // transição completou: renovar timer local da bomba (§4.2)
            break;
        case State::PUMP_OFF_WAIT:
            g.pump = false;
            g.state = State::DRAIN;
            break;
        case State::CLOSE_LAST_WAIT:
            g.state = State::IDLE;
            g.curZone = 0;
            g.reachedRunning = false; // ciclo encerrado
            break;
        default:
            break;
        }
        return;
    }
}

// NACK: a estação REJEITOU o comando (safe mode, bateria baixa, rate limit). É uma FALHA,
// não um sucesso — encaminha para o mesmo tratamento de onCmdFailed. Localiza o pend por
// (node, seq), captura zoneId/action e delega a onCmdFailed(node, zoneId, action), que
// re-encontra o mesmo pend (casa por node+zoneId+action) e o limpa. A dupla-busca é segura:
// entre onNack e onCmdFailed nada mais mexe no pend.
void HydraulicGroupEngine::onNack(uint32_t node, uint32_t ackedSeq)
{
    for (size_t i = 0; i < MAX_GROUPS; i++) {
        GroupRt &g = rt[i];
        if (!g.pend.inUse || g.pend.node != node || g.pend.seq != ackedSeq)
            continue;
        uint8_t zoneId = g.pend.zoneId, action = g.pend.action;
        onCmdFailed(node, zoneId, action);
        return;
    }
}

// Casamento por (node, zoneId, action) e não por seq: uma zona-membro pertence a
// no máximo UM grupo (HydraulicGroupTable::valid rejeita compartilhamento), então
// (node, zoneId) identifica um único pend de válvula. Compartilhar a mesma zona de
// BOMBA entre grupos é misconfig não suportada. seq não é exposto pelo CommandTracker
// no caminho FAILED, por isso não é usado aqui (onAck usa seq porque o tem).
void HydraulicGroupEngine::onCmdFailed(uint32_t node, uint8_t zoneId, uint8_t action)
{
    for (size_t i = 0; i < MAX_GROUPS; i++) {
        GroupRt &g = rt[i];
        uint8_t groupId = (uint8_t)(i + 1);
        if (!g.pend.inUse || g.pend.node != node || g.pend.zoneId != zoneId || g.pend.action != action)
            continue;
        g.pend.inUse = false;
        if (action == 1) {
            g.openFailZone = zoneId;
            g.openFailPending = true;
            g.state = State::RUNNING;
        } else { // action == 0: fechar falhou
            pushAlert(groupId, GA_CLOSE_FAIL, zoneId);
            g.closeFailStreak++;
            // Válvula extra aberta = pressão menor = seguro: NÃO desligar a bomba.
            // Limpa o estado de transição pendente e retorna a RUNNING.
            g.curZone = g.nextZone ? g.nextZone : g.curZone;
            g.nextZone = 0;
            g.state = State::RUNNING;
        }
        return;
    }
}
void HydraulicGroupEngine::observeActual(uint8_t groupId, uint8_t zoneId, bool open)
{
    if (groupId == 0 || groupId > MAX_GROUPS)
        return;
    GroupRt &g = rtOf(groupId);
    ZoneRt *z = findZone(g, zoneId);
    if (!z)
        return;
    z->confirmed = open;
    if (!open)
        z->localExpiresMs = 0;
    g.reconcileCheck = true; // tick decide se o actual-set caiu abaixo de minOpen
}

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

uint8_t HydraulicGroupEngine::currentZone(uint8_t groupId) const
{
    if (groupId < 1 || groupId > MAX_GROUPS)
        return 0;
    return rt[groupId - 1].curZone;
}

uint8_t HydraulicGroupEngine::openConfirmedCount(uint8_t groupId) const
{
    if (groupId < 1 || groupId > MAX_GROUPS)
        return 0;
    const GroupRt &g = rt[groupId - 1];
    uint8_t n = 0;
    for (size_t i = 0; i < MAX_ZONES; i++)
        if (g.zones[i].zoneId != 0 && g.zones[i].confirmed)
            n++;
    return n;
}

uint8_t HydraulicGroupEngine::pruneStarts(GroupRt &g, uint32_t nowMs) const
{
    uint8_t w = 0;
    for (uint8_t i = 0; i < g.startCount; i++)
        if ((int32_t)(nowMs - g.startRing[i]) <= (int32_t)START_WINDOW_MS)
            g.startRing[w++] = g.startRing[i];
    g.startCount = w;
    return w;
}

void HydraulicGroupEngine::registerStart(GroupRt &g, uint32_t nowMs)
{
    pruneStarts(g, nowMs);
    if (g.startCount < 8)
        g.startRing[g.startCount++] = nowMs;
    g.lastStartMs = nowMs;
}

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
            // Guarda max_partidas_hora: se o budget de partidas estiver esgotado, adia.
            if (cfg->bombaZoneId != 0 && cfg->maxStartsHour != 0 &&
                pruneStarts(g, nowMs) >= cfg->maxStartsHour) {
                g.state = State::DEFERRED;
                pushAlert(groupId, GA_DEFER_RATE, 0);
                break; // NÃO abre válvula sem bomba
            }
            if (!resolve(zones, w->zoneId, e))
                break;
            e.action = 1;
            e.durationS = w->wantDurS;
            g.pend = {true, e.node, 0, w->zoneId, 1};
            g.curZone = w->zoneId;
            if (ZoneRt *zr = findZone(g, w->zoneId))
                zr->localExpiresMs = nowMs + (uint32_t)e.durationS * 1000; // deadline local (Task 5)
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
                if (w) {
                    if (!resolve(zones, w->zoneId, e))
                        break;
                    e.action = 1;
                    e.durationS = w->wantDurS;
                    g.pend = {true, e.node, 0, w->zoneId, 1};
                    g.curZone = w->zoneId;
                    if (ZoneRt *zr = findZone(g, w->zoneId))
                        zr->localExpiresMs = nowMs + (uint32_t)e.durationS * 1000; // deadline local (Task 5)
                    g.waitStartMs = nowMs; // re-ancora START_WAIT no emit do próximo open
                    g.state = State::OPENING;
                    out[emitted++] = e;
                    break;
                }
                // !w: nada mais a abrir — segue com o que tem (evita travar; cai no timer abaixo)
            }
            // §8.10 simultaneidade: grupo sem bomba vai direto a RUNNING assim que minOpen confirmadas.
            // Não espera startAfterOpenS (não há bomba a proteger) e não emite GPO de bomba.
            if (cfg->bombaZoneId == 0) {
                g.state = State::RUNNING;
                break;
            }
            // Timer ancorado no INSTANTE DO EMIT do open (não no ACK): mede partida_apos_abrir_s
            // desde o envio do comando. Para ACK local rápido é equivalente; NÃO re-ancorar no onAck.
            if (nowMs - g.waitStartMs < (uint32_t)cfg->startAfterOpenS * 1000)
                break;
            if (!resolve(zones, cfg->bombaZoneId, e))
                break;
            e.action = 1;
            uint16_t maxDur = 0;
            for (auto &zz : g.zones)
                if (zz.zoneId && zz.confirmed && zz.wantDurS > maxDur)
                    maxDur = zz.wantDurS;
            e.durationS = pumpDur(maxDur ? maxDur : 600u);
            g.pend = {true, e.node, 0, cfg->bombaZoneId, 1};
            g.state = State::PUMP_WAIT_ACK;
            registerStart(g, nowMs); // registra partida da bomba (max_partidas_hora + bridging)
            pushAlert(groupId, GA_PUMP_ON, cfg->bombaZoneId);
            out[emitted++] = e;
            break;
        }
        case State::RUNNING: {
            if (g.openFailPending) {
                // A partida da bomba em si falhou (PUMP_WAIT_ACK nunca completou -> g.pump==false):
                // não há bomba a proteger e não dá p/ "renovar a corrente" (não há corrente com bomba).
                // Aborta o ciclo com segurança: cancela o desejo e vai a DRAIN fechar as válvulas.
                if (g.openFailZone == cfg->bombaZoneId && cfg->bombaZoneId != 0) {
                    pushAlert(groupId, GA_OPEN_FAIL_PUMPOFF, g.openFailZone);
                    g.openFailPending = false;
                    for (auto &zz : g.zones)
                        if (zz.zoneId)
                            zz.wanted = false; // ninguém mais desejado: DRAIN fecha as confirmadas
                    g.waitStartMs = nowMs; // âncora do DRAIN (parar_antes_de_fechar_s)
                    g.state = State::DRAIN; // g.pump já é false: sem parada de bomba a fazer
                    break;
                }
                ZoneRt *cur = findZone(g, g.curZone);
                bool nearDeadline = cur && cur->localExpiresMs != 0 &&
                                    (int32_t)(cur->localExpiresMs - nowMs) < (int32_t)RENEW_MARGIN_MS;
                if (nearDeadline && g.pump && cfg->bombaZoneId != 0) {
                    // não dá p/ renovar a tempo -> desliga a bomba primeiro; a corrente fecha pelo timer local.
                    if (!resolve(zones, cfg->bombaZoneId, e)) { g.openFailPending = false; pushAlert(groupId, GA_OPEN_FAIL_PUMPOFF, g.openFailZone); break; }
                    e.action = 0;
                    g.pend = {true, e.node, 0, cfg->bombaZoneId, 0};
                    g.state = State::PUMP_OFF_WAIT;
                    g.openFailPending = false;
                    pushAlert(groupId, GA_OPEN_FAIL_PUMPOFF, g.openFailZone);
                    out[emitted++] = e;
                    break;
                }
                // renova a corrente: re-abre p/ reiniciar o timer local; retenta a próxima depois.
                // A cadência de retentativa da próxima zona é dada pelo ciclo do CommandTracker no glue
                // (onCmdFailed só dispara após esgotar os retries ~24s) + o gate de 1 comando em voo por
                // grupo. O engine não faz backoff próprio; não chamar tick em loop apertado sem esse ciclo.
                if (!resolve(zones, g.curZone, e)) { g.openFailPending = false; pushAlert(groupId, GA_OPEN_FAIL_RENEW, g.openFailZone); break; }
                e.action = 1;
                e.durationS = cur ? cur->wantDurS : 600u;
                g.pend = {true, e.node, 0, g.curZone, 1};
                if (cur)
                    cur->localExpiresMs = nowMs + (uint32_t)e.durationS * 1000;
                g.openFailPending = false;
                // IMPORTANTE: NÃO ir a OPENING/START_WAIT (re-dispararia a partida da bomba já ligada).
                // Fica em RUNNING: o ACK do re-open (default case do onAck) só reconfirma a corrente;
                // o próximo tick em RUNNING acha a próxima zona (ainda wanted+unconfirmed) e retenta.
                g.state = State::RUNNING;
                pushAlert(groupId, GA_OPEN_FAIL_RENEW, g.openFailZone);
                out[emitted++] = e;
                break;
            }
            // Ordered shutdown: falhas de fechamento persistentes com maxOpen excedido.
            if (cfg->maxOpen != 0 && g.closeFailStreak > cfg->maxOpen && g.pump) {
                if (resolve(zones, cfg->bombaZoneId, e)) {
                    e.action = 0;
                    g.pend = {true, e.node, 0, cfg->bombaZoneId, 0};
                    g.waitStartMs = nowMs;
                    g.state = State::PUMP_OFF_WAIT;
                    pushAlert(groupId, GA_ORDERED_SHUTDOWN, 0);
                    g.closeFailStreak = 0;
                    out[emitted++] = e;
                }
                break;
            }
            // §8.13 reconciliação por heartbeat: observeActual sinalizou mudança de estado.
            // Se o actual-set caiu abaixo de minOpen enquanto a bomba está ligada, faz parada
            // ordenada (GA_REBOOT_RECONCILE) — cobre estação reiniciando com válvula fechada.
            if (g.reconcileCheck) {
                g.reconcileCheck = false;
                if (g.reachedRunning && g.pump && cfg->bombaZoneId != 0 &&
                    confirmedCount(g) < (size_t)cfg->minOpen) {
                    if (resolve(zones, cfg->bombaZoneId, e)) {
                        e.action = 0;
                        g.pend = {true, e.node, 0, cfg->bombaZoneId, 0};
                        g.state = State::PUMP_OFF_WAIT;
                        out[emitted++] = e;
                    }
                    pushAlert(groupId, GA_REBOOT_RECONCILE, 0);
                    break;
                }
            }
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
                if (ZoneRt *zr = findZone(g, w->zoneId))
                    zr->localExpiresMs = nowMs + (uint32_t)e.durationS * 1000; // deadline local (Task 5)
                g.waitStartMs = nowMs;        // âncora do X_OVERLAP (sobreposicao_s)
                g.state = State::X_OPEN_WAIT; // abrir_antes_de_fechar
                out[emitted++] = e;
                break;
            }
            if (!anyWanted()) {                          // fim: desliga bomba
                if (cfg->bombaZoneId != 0 && g.pump) {
                    // Bridging: se minRunMin não expirou, mantém bomba ligada (ponte)
                    if (cfg->minRunMin != 0 &&
                        (uint32_t)(nowMs - g.lastStartMs) < (uint32_t)cfg->minRunMin * 60000u)
                        break; // ponte: mantém bomba ligada; retoma sem nova partida
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
            // (3) steady: renova a bomba se uma transição acabou de completar (restaura timer local, §4.2).
            if (g.pump && g.pumpNeedsRenew && cfg->bombaZoneId != 0) {
                uint16_t maxDur = 0;
                for (auto &zz : g.zones)
                    if (zz.zoneId && zz.confirmed && zz.wantDurS > maxDur)
                        maxDur = zz.wantDurS;
                if (!resolve(zones, cfg->bombaZoneId, e))
                    break;
                e.action = 1;
                e.durationS = pumpDur(maxDur ? maxDur : 600u);
                g.pend = {true, e.node, 0, cfg->bombaZoneId, 1};
                g.pumpNeedsRenew = false;
                g.state = State::PUMP_WAIT_ACK; // ao ACK: pump segue true, volta a RUNNING (sem novo GA_PUMP_ON)
                out[emitted++] = e;
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
                g.pumpNeedsRenew = true; // transição completou: renovar timer local da bomba (§4.2)
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
                g.reachedRunning = false; // ciclo encerrado
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
        case State::DEFERRED:
            // Aguarda o budget de partidas liberar; no próximo tick em IDLE reabre as zonas.
            if (cfg->maxStartsHour == 0 || pruneStarts(g, nowMs) < cfg->maxStartsHour)
                g.state = State::IDLE;
            break;
        default:
            break;
        }
    }
    return emitted;
}
