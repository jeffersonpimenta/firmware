#include "GpoController.h"

GpoController::GpoController(IGpoDriver &driver, uint8_t numGpos)
    : driver(driver), numGpos(numGpos > MAX_GPO ? MAX_GPO : numGpos)
{
}

GpoController::Result GpoController::command(uint8_t id, uint8_t action, uint16_t durationS, uint32_t nowMs)
{
    if (id >= numGpos)
        return Result::INVALID_ID;

    if (action) {
        uint32_t deadline = durationS ? nowMs + (uint32_t)durationS * 1000u : 0u;
        // Sentinel: offAtMs==0 significa biestável. Se o cálculo do deadline resultar
        // em zero (raro, apenas no wrap de millis()), forçamos 1 para preservar o sentinela.
        if (durationS && deadline == 0u)
            deadline = 1u;
        if (!slots[id].on)
            driver.set(id, true); // já ligado: só renova o timer, sem re-acionar o driver
        slots[id].on = true;
        slots[id].offAtMs = deadline;
    } else {
        driver.set(id, false);
        slots[id].on = false;
        slots[id].offAtMs = 0;
    }
    return Result::OK;
}

void GpoController::tick(uint32_t nowMs)
{
    for (uint8_t i = 0; i < numGpos; i++) {
        // offAtMs==0: biestável, não expira. Diferença signed: segura contra rollover de millis().
        if (slots[i].on && slots[i].offAtMs != 0 && (int32_t)(nowMs - slots[i].offAtMs) >= 0) {
            driver.set(i, false);
            slots[i].on = false;
            slots[i].offAtMs = 0;
        }
    }
}

void GpoController::allOff()
{
    for (uint8_t i = 0; i < numGpos; i++) {
        if (slots[i].on) {
            driver.set(i, false);
            slots[i].on = false;
        }
        slots[i].offAtMs = 0;
    }
}

uint8_t GpoController::states() const
{
    uint8_t bm = 0;
    for (uint8_t i = 0; i < numGpos; i++)
        if (slots[i].on)
            bm |= (1u << i);
    return bm;
}

void GpoController::setNumGpos(uint8_t n)
{
    if (n > MAX_GPO)
        n = MAX_GPO;
    // Encolheu: desliga fisicamente os GPOs removidos
    for (uint8_t i = n; i < numGpos; i++) {
        driver.set(i, false);
        slots[i].on = false;
        slots[i].offAtMs = 0;
    }
    // Cresceu: estado físico desconhecido após reconfig em runtime → força desligado
    for (uint8_t i = numGpos; i < n; i++) {
        driver.set(i, false);
        slots[i] = Slot{};
    }
    numGpos = n;
}
