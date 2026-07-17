#include "modules/irrigation/PortalSession.h"

void PortalSession::requestOpen(uint32_t nowMs)
{
    _state = State::OPEN;
    _lastActivityMs = nowMs;
}

void PortalSession::noteClient(uint32_t nowMs, bool anyClient)
{
    if (_state == State::OPEN && anyClient)
        _lastActivityMs = nowMs;
}

void PortalSession::tick(uint32_t nowMs)
{
    if (_state != State::OPEN)
        return;
    if (nowMs - _lastActivityMs >= PORTAL_TIMEOUT_MS)
        _state = State::CLOSED;
}

uint32_t PortalSession::secondsLeft(uint32_t nowMs) const
{
    if (_state != State::OPEN)
        return 0;
    uint32_t elapsed = nowMs - _lastActivityMs;
    if (elapsed >= PORTAL_TIMEOUT_MS)
        return 0;
    return (PORTAL_TIMEOUT_MS - elapsed) / 1000;
}
