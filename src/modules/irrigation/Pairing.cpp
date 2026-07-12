#include "Pairing.h"

void StationPairing::openWindow(uint32_t nowMs)
{
    if (st == State::COMMITTED)
        return;
    st = State::WINDOW;
    windowStartMs = nowMs;
    announced = false;
}

void StationPairing::tick(uint32_t nowMs)
{
    if (st == State::WINDOW && (nowMs - windowStartMs) > WINDOW_MS)
        st = State::IDLE;
}

bool StationPairing::announceDue(uint32_t nowMs)
{
    tick(nowMs);
    if (st != State::WINDOW)
        return false;
    if (announced && (nowMs - lastAnnounceMs) < ANNOUNCE_INTERVAL_MS)
        return false;
    announced = true;
    lastAnnounceMs = nowMs;
    return true;
}

bool StationPairing::onGrant(const IrrigationProto::PairGrant &g, uint32_t nowMs)
{
    tick(nowMs);
    if (st != State::WINDOW)
        return false;
    granted = g;
    st = State::COMMITTED;
    return true;
}

void GatewayPairing::openWindow(uint32_t nowMs)
{
    open = true;
    windowStartMs = nowMs;
}

void GatewayPairing::tick(uint32_t nowMs)
{
    if (open && (nowMs - windowStartMs) > WINDOW_MS)
        open = false;
}

bool GatewayPairing::approveAnnounce(uint32_t nodeId, uint32_t nowMs)
{
    tick(nowMs);
    if (!open)
        return false;
    for (auto &r : recent) {
        if (r.node == nodeId && (nowMs - r.atMs) < REGRANT_COOLDOWN_MS)
            return false;
    }
    // registra (substitui a entrada mais antiga)
    Recent *slot = &recent[0];
    for (auto &r : recent)
        if (r.atMs < slot->atMs)
            slot = &r;
    slot->node = nodeId;
    slot->atMs = nowMs;
    return true;
}
