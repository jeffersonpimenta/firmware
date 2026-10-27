#include "modules/irrigation/InputActuationPolicy.h"

void InputActuationPolicy::configure(InputMode mode, bool activeLow)
{
    _mode = mode;
    _activeLow = activeLow;
    _lastRaw = false;
    _sinceMs = 0;
    _stable = false;
    _armed = true;
    _lastRenewMs = 0;
}

InputActuationPolicy::Intent InputActuationPolicy::update(bool rawHigh, uint32_t nowMs)
{
    if (_mode == InputMode::DISABLED)
        return Intent::NONE;

    const bool pressed = _activeLow ? !rawHigh : rawHigh;

    // Track leading edge of any raw change for debounce timing.
    if (pressed != _lastRaw) {
        _lastRaw = pressed;
        _sinceMs = nowMs;
    }

    // Debounce: only commit a state change after the signal is stable for debounceMs().
    if ((uint32_t)(nowMs - _sinceMs) >= debounceMs() && pressed != _stable) {
        _stable = pressed;

        if (_stable) {
            // Stable rising edge.
            _lastRenewMs = nowMs;
            switch (_mode) {
            case InputMode::FOLLOW:
                // Level-follow: rising edge always fires OPEN (no arming needed).
                return Intent::OPEN;

            case InputMode::EDGE_BOTH:
                // Both-edge: rising edge fires OPEN (no arming needed).
                return Intent::OPEN;

            case InputMode::EDGE_TOGGLE:
                // Single-fire per press: only fire when armed.
                if (_armed) {
                    _armed = false;
                    return Intent::TOGGLE;
                }
                break;

            case InputMode::EDGE_OPEN:
                if (_armed) {
                    _armed = false;
                    return Intent::OPEN;
                }
                break;

            case InputMode::EDGE_CLOSE:
                if (_armed) {
                    _armed = false;
                    return Intent::CLOSE;
                }
                break;

            default:
                break;
            }
        } else {
            // Stable falling edge: rearm single-fire modes; FOLLOW/EDGE_BOTH also emit CLOSE.
            _armed = true;
            if (_mode == InputMode::FOLLOW || _mode == InputMode::EDGE_BOTH)
                return Intent::CLOSE;
        }
    }

    // Renewal: FOLLOW re-emits OPEN every RENEW_MS while the input is held active.
    if (_mode == InputMode::FOLLOW && _stable && (uint32_t)(nowMs - _lastRenewMs) >= RENEW_MS) {
        _lastRenewMs = nowMs;
        return Intent::OPEN;
    }

    return Intent::NONE;
}
