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
        if (shortArmed && (nowMs - shortArmedMs) <= DOUBLE_GAP_MS) {
            shortArmed = false;
            doubleDetected = true;
            ev = Event::DOUBLE;
        } else {
            doubleDetected = false;
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
