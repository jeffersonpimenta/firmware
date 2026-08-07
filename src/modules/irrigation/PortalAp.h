#pragma once
// Guarda idêntica à do WebServerThread do ESP32 (main.cpp:1134). Precisa de ARCH_ESP32 porque o
// chamador (IrrigationModule::runOnce) É compilado no build nativo — MESHTASTIC_EXCLUDE_WEBSERVER
// NÃO é definido em native (ARCH_PORTDUINO); só o ARCH_ESP32 evita link error contra o .cpp filtrado.
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER

#include <stdint.h>

// Cola só-ESP32 do captive portal (Fase 5b). Dirigida pela PortalSession do módulo:
// quando apShouldBeUp() vira true, sobe softAP (WPA2) + DNSServer cativo; quando vira false,
// derruba tudo. Chamar portalApLoop() periodicamente (do runOnce do módulo).
void portalApLoop(unsigned long nowMs); // unsigned long (= millis()) evita mismatch de mangling uint32_t entre TUs

#endif
