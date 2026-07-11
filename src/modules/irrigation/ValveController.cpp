#include "ValveController.h"

ValveController::Result ValveController::open(uint8_t id, uint32_t durationS, uint32_t configMaxS, uint32_t nowMs)
{
    if (id >= numValves)
        return Result::INVALID_ID;
    if (durationS == 0)
        return Result::ZERO_DURATION;
    if (batteryLockout)
        return Result::BATTERY_LOW;

    uint32_t effectiveS = durationS;
    if (configMaxS != 0 && configMaxS < effectiveS)
        effectiveS = configMaxS;
    if (effectiveS > MAX_OPEN_SECONDS)
        effectiveS = MAX_OPEN_SECONDS;

    if (!slots[id].open) {
        driver.pulse(id, true);
        slots[id].open = true;
    }
    slots[id].closeAtMs = nowMs + effectiveS * 1000u;
    return Result::OK;
}

ValveController::Result ValveController::close(uint8_t id)
{
    if (id >= numValves)
        return Result::INVALID_ID;
    if (slots[id].open) {
        driver.pulse(id, false);
        slots[id].open = false;
    }
    return Result::OK;
}

void ValveController::closeAll()
{
    for (uint8_t i = 0; i < numValves; i++)
        close(i);
}

void ValveController::forceCloseAll()
{
    for (uint8_t i = 0; i < numValves; i++) {
        driver.pulse(i, false);
        slots[i].open = false;
    }
}

void ValveController::tick(uint32_t nowMs)
{
    for (uint8_t i = 0; i < numValves; i++) {
        // Diferença signed: segura contra rollover de millis()
        if (slots[i].open && (int32_t)(nowMs - slots[i].closeAtMs) >= 0)
            close(i);
    }
}

void ValveController::setNumValves(uint8_t n)
{
    if (n > MAX_VALVES)
        n = MAX_VALVES;
    for (uint8_t i = n; i < numValves; i++) { // encolheu: fecha removidas
        driver.pulse(i, false);
        slots[i].open = false;
    }
    for (uint8_t i = numValves; i < n; i++) { // cresceu: novas em estado desconhecido
        driver.pulse(i, false);
        slots[i].open = false;
    }
    numValves = n;
}

uint8_t ValveController::stateBitmap() const
{
    uint8_t bm = 0;
    for (uint8_t i = 0; i < numValves; i++)
        if (slots[i].open)
            bm |= (1u << i);
    return bm;
}
