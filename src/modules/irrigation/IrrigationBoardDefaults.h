#pragma once
#include "IrrigationSettings.h"

// Aplica o mapa de pinos definido pelo variant da board (macros IRRIGATION_PIN_*).
// No-op se o build não definir nenhuma macro (nativo, boards não-irrigação) — nesses
// casos a config vem do portal/gateway como antes. Só mexe em campos de pino/contagem;
// nunca em role/boundGateway/configEpoch/tabelas.
static inline void applyBoardIrrigationDefaults(IrrigationSettings &s)
{
#if defined(IRRIGATION_PIN_VALVE0_A)
#ifdef IRRIGATION_NUM_VALVES
    s.numValves = IRRIGATION_NUM_VALVES;
#endif
    s.pinsHbridgeA[0] = IRRIGATION_PIN_VALVE0_A;
    s.pinsHbridgeB[0] = IRRIGATION_PIN_VALVE0_B;
#ifdef IRRIGATION_PIN_VALVE1_A
    s.pinsHbridgeA[1] = IRRIGATION_PIN_VALVE1_A;
    s.pinsHbridgeB[1] = IRRIGATION_PIN_VALVE1_B;
#endif
#ifdef IRRIGATION_PIN_DIN0
    s.pinsDigitalIn[0] = IRRIGATION_PIN_DIN0;
#endif
#ifdef IRRIGATION_PIN_DIN1
    s.pinsDigitalIn[1] = IRRIGATION_PIN_DIN1;
#endif
#ifdef IRRIGATION_DIN_ACTIVE_LOW
    s.digitalInActiveLow = IRRIGATION_DIN_ACTIVE_LOW;
#endif
#ifdef IRRIGATION_PIN_TAMPER
    s.pinTamper = IRRIGATION_PIN_TAMPER;
#if defined(IRRIGATION_TAMPER_ACTIVE_LOW) && IRRIGATION_TAMPER_ACTIVE_LOW
    s.hwFlags |= 0x01;
#endif
#endif
#ifdef IRRIGATION_PIN_GPO0
    s.pinsGpo[0] = IRRIGATION_PIN_GPO0;
#endif
#ifdef IRRIGATION_PIN_GPO1
    s.pinsGpo[1] = IRRIGATION_PIN_GPO1;
#endif
#ifdef IRRIGATION_PIN_BTN
    s.pinBtn = IRRIGATION_PIN_BTN;
#endif
#ifdef IRRIGATION_PIN_LED
    s.pinLed = IRRIGATION_PIN_LED;
#endif
#ifdef IRRIGATION_PIN_SENSOR0
    s.sensores[0].pino = IRRIGATION_PIN_SENSOR0;
    s.sensores[0].tipo = 1; // analógico
#endif
#else
    (void)s; // nenhuma macro de board → no-op
#endif
}
