#pragma once
#include <stdint.h>

// Rate limit de comandos no receptor (spec §4.2). Janela fixa de 60 s.
class RateLimiter
{
  public:
    explicit RateLimiter(uint8_t maxPerMinute) : maxPerMinute(maxPerMinute) {}
    bool allow(uint32_t nowMs);

  private:
    uint8_t maxPerMinute;
    uint32_t windowStartMs = 0;
    uint8_t count = 0;
    bool started = false;
};
