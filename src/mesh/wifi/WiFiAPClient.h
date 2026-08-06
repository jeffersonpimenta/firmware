#pragma once

#include "concurrency/Periodic.h"
#include "configuration.h"
#include <Arduino.h>
#include <functional>

#if HAS_WIFI && !defined(ARCH_PORTDUINO)
#include <WiFi.h>
#endif

#if HAS_ETHERNET && defined(ARCH_ESP32)
#include <ETH.h>
#endif // HAS_ETHERNET

extern bool needReconnect;
extern concurrency::Periodic *wifiReconnect;

/// @return true if wifi is now in use
bool initWifi();

void deinitWifi();

bool isWifiAvailable();

uint8_t getWifiDisconnectReason();

// Fase 8b (irrigação): força um novo fetch NTP no próximo tick (zera o throttle).
// No-op se DISABLE_NTP. Seguro chamar quando WiFi não está conectado (fetch só corre com STA up).
void triggerNtpUpdate();
// millis() do último NTP set bem-sucedido; 0 se nunca sincronizou.
unsigned long ntpLastRunMs();

#if defined(USE_WS5500) || defined(USE_CH390D)
// Startup Ethernet
bool initEthernet();
#endif