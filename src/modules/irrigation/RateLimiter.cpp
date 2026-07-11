#include "RateLimiter.h"

bool RateLimiter::allow(uint32_t nowMs)
{
    // Subtração unsigned: segura contra rollover de millis()
    if (!started || (nowMs - windowStartMs) >= 60000u) {
        started = true;
        windowStartMs = nowMs;
        count = 0;
    }
    if (count >= maxPerMinute)
        return false;
    count++;
    return true;
}
