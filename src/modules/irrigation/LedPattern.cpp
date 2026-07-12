#include "LedPattern.h"

namespace
{
constexpr uint32_t WINDOW_MS = 5000;
constexpr uint32_t BLINK_ON_MS = 100, BLINK_PERIOD_MS = 250; // on 100, gap 150

bool blinkPattern(uint32_t t, uint8_t count)
{
    if (t >= (uint32_t)count * BLINK_PERIOD_MS)
        return false;
    return (t % BLINK_PERIOD_MS) < BLINK_ON_MS;
}

// SOS: 3 pontos (150/150), 3 traços (450/150), 3 pontos (150/150)
bool sosPattern(uint32_t t)
{
    struct Seg {
        uint32_t on, off;
        uint8_t reps;
    };
    static const Seg segs[] = {{150, 150, 3}, {450, 150, 3}, {150, 150, 3}};
    for (const auto &s : segs) {
        for (uint8_t r = 0; r < s.reps; r++) {
            if (t < s.on)
                return true;
            t -= s.on;
            if (t < s.off)
                return false;
            t -= s.off;
        }
    }
    return false; // pausa até o fim da janela
}
} // namespace

bool LedPatternController::ledOn(uint32_t nowMs) const
{
    uint32_t t = nowMs % WINDOW_MS;
    switch (mode) {
    case Mode::NORMAL:
        return blinkPattern(t, 1);
    case Mode::NO_GATEWAY:
        return blinkPattern(t, 2);
    case Mode::CONFIG_PENDING:
        return blinkPattern(t, 3);
    case Mode::PAIRING:
        return (nowMs % 200) < 100;
    case Mode::OUTPUT_OPEN:
        return true;
    case Mode::BATTERY_SOS:
        return sosPattern(t);
    }
    return false;
}
