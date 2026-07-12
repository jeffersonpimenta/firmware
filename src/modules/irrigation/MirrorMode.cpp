#include "MirrorMode.h"

MirrorMode::MirrorMode() : _enabled(false)
{
    for (size_t i = 0; i < INPUTS; i++) {
        _inputs[i] = {};
    }
}

void MirrorMode::setEnabled(bool e)
{
    _enabled = e;
}

bool MirrorMode::enabled() const
{
    return _enabled;
}

MirrorMode::Action MirrorMode::update(uint8_t inputsBitmap, uint32_t nowMs)
{
    Action result = {Action::T::NONE, 0};

    // If disabled, drain all active inputs and then ignore
    if (!_enabled) {
        for (uint8_t i = 0; i < INPUTS; i++) {
            if (_inputs[i].stable && _inputs[i].needClose == false) {
                _inputs[i].needClose = true;
                result.t = Action::T::CLOSE;
                result.input = i;
                return result;
            }
        }
        // All drains done, ignore everything
        return result;
    }

    // Enabled: process inputs
    // Priority: needClose (drain) → edges (rise/fall) → renewals

    // Step 1: Handle needClose (drain from disable-drains)
    for (uint8_t i = 0; i < INPUTS; i++) {
        if (_inputs[i].needClose) {
            _inputs[i].needClose = false;
            _inputs[i].stable = false;
            result.t = Action::T::CLOSE;
            result.input = i;
            return result;
        }
    }

    // Step 2: Process debounce and edges
    for (uint8_t i = 0; i < INPUTS; i++) {
        bool currentRaw = (inputsBitmap >> i) & 1;

        // If raw value changed, reset the timer FIRST (before computing elapsed time)
        if (currentRaw != _inputs[i].rawLast) {
            _inputs[i].rawLast = currentRaw;
            _inputs[i].rawSinceMs = nowMs;
        }

        uint32_t timeSinceChange = nowMs - _inputs[i].rawSinceMs;

        // If enough time has passed, transition to stable state
        if (timeSinceChange >= DEBOUNCE_MS) {
            if (!_inputs[i].stable && currentRaw) {
                // Rising edge: input became active after debounce
                _inputs[i].stable = true;
                _inputs[i].lastRenewMs = nowMs;
                result.t = Action::T::OPEN;
                result.input = i;
                return result;
            } else if (_inputs[i].stable && !currentRaw) {
                // Falling edge: input became inactive after debounce
                _inputs[i].stable = false;
                result.t = Action::T::CLOSE;
                result.input = i;
                return result;
            }
        }
    }

    // Step 3: Process renewals
    for (uint8_t i = 0; i < INPUTS; i++) {
        if (_inputs[i].stable) {
            if (nowMs - _inputs[i].lastRenewMs >= RENEW_MS) {
                _inputs[i].lastRenewMs = nowMs;
                result.t = Action::T::OPEN;
                result.input = i;
                return result;
            }
        }
    }

    return result;
}

bool MirrorMode::inputActive(uint8_t input) const
{
    if (input >= INPUTS)
        return false;
    return _inputs[input].stable;
}

size_t MirrorMode::serialize(uint8_t *buf, size_t cap) const
{
    // Format: magic(4) + version(1) + enabled(1) = 6 bytes
    if (cap < 6)
        return 0;

    uint32_t magic = MAGIC;
    buf[0] = magic & 0xFF;
    buf[1] = (magic >> 8) & 0xFF;
    buf[2] = (magic >> 16) & 0xFF;
    buf[3] = (magic >> 24) & 0xFF;
    buf[4] = 1; // version
    buf[5] = _enabled ? 1 : 0;

    return 6;
}

bool MirrorMode::deserialize(const uint8_t *buf, size_t n)
{
    if (n < 6)
        return false;

    uint32_t magic = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    if (magic != MAGIC)
        return false;

    uint8_t version = buf[4];
    if (version != 1)
        return false;

    _enabled = (buf[5] != 0);
    return true;
}
