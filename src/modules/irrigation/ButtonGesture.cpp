#include "ButtonGesture.h"

ButtonGestureDetector::Event ButtonGestureDetector::update(bool pressed, uint32_t nowMs)
{
    if (pressed != rawLast) {
        rawLast = pressed;
        rawSinceMs = nowMs;
    }
    bool debounced = stable;
    if ((nowMs - rawSinceMs) >= DEBOUNCE_MS)
        debounced = rawLast;

    Event ev = Event::NONE;

    if (debounced && !stable) { // borda de pressão
        doubleDetected = false;
        if (shortArmed && (nowMs - shortArmedMs) <= DOUBLE_GAP_MS) {
            shortArmed = false;
            doubleDetected = true;
            ev = Event::DOUBLE;
        }
        pressStartMs = nowMs;
        longFired = holdFired = false;
    } else if (!debounced && stable) { // borda de soltura
        uint32_t held = nowMs - pressStartMs;
        if (!longFired && !holdFired && held < SHORT_MAX_MS && ev == Event::NONE && !doubleDetected) {
            shortArmed = true;
            shortArmedMs = nowMs;
        }
    } else if (debounced) { // segurando
        uint32_t held = nowMs - pressStartMs;
        // A ordem if/else-if é intencional: aos 10 s, longFired já é true, então o
        // primeiro ramo é falso e o else-if do HOLD dispara. Não "consertar".
        if (!longFired && held >= LONG_MS) {
            longFired = true;
            ev = Event::LONG_3S;
        } else if (!holdFired && held >= HOLD_MS) {
            holdFired = true;
            ev = Event::HOLD_10S;
        }
    } else { // solto
        if (shortArmed && (nowMs - shortArmedMs) > DOUBLE_GAP_MS) {
            shortArmed = false;
            ev = Event::SHORT;
        }
    }
    stable = debounced;
    return ev;
}
