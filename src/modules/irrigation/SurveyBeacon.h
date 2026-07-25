#pragma once
#include <cstddef>
#include <cstdint>

// §8.5 site survey: beacon-mode state machine. Pure, no hardware.
// A node in beacon mode emits PING_SURVEY (kind=2) every intervalS seconds
// until timeoutS elapses. tick() tells the caller when to emit.
class SurveyBeacon
{
  public:
    void start(uint32_t nowMs, uint16_t intervalS, uint16_t timeoutS, int32_t latE7v, int32_t lonE7v, bool hasCoordV)
    {
        on = true;
        intervalMs = (uint32_t)intervalS * 1000;
        expireAtMs = nowMs + (uint32_t)timeoutS * 1000;
        nextEmitMs = nowMs; // primeiro beacon imediato
        lat = latE7v;
        lon = lonE7v;
        coord = hasCoordV;
    }
    void stop() { on = false; }
    bool active(uint32_t nowMs) const { return on && nowMs < expireAtMs; }
    // true no máximo 1×/intervalo enquanto ativo; avança o cronograma; auto-para no timeout.
    bool tick(uint32_t nowMs)
    {
        if (!on)
            return false;
        if (nowMs >= expireAtMs) {
            on = false;
            return false;
        }
        if (nowMs < nextEmitMs)
            return false;
        nextEmitMs += intervalMs ? intervalMs : 1000;
        return true;
    }
    int32_t latE7() const { return lat; }
    int32_t lonE7() const { return lon; }
    bool hasCoord() const { return coord; }

  private:
    bool on = false;
    uint32_t nextEmitMs = 0, expireAtMs = 0, intervalMs = 0;
    int32_t lat = 0, lon = 0;
    bool coord = false;
};
