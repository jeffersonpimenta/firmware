#include "configuration.h" // ARCH_ESP32 é macro de header (não -D): precisa vir ANTES do guard abaixo
#include "modules/irrigation/PortalAp.h"
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER

#include "main.h" // owner
#include "modules/irrigation/IrrigationModule.h"
#include <DNSServer.h>
#include <WiFi.h>

static bool sApUp = false;
static DNSServer sDns;

// PIN de aplicação do portal (spec §7). Placeholder compilado; PIN configurável via config = follow-up.
// WPA2 exige >= 8 chars.
#ifndef IRRIGATION_PORTAL_PIN
#define IRRIGATION_PORTAL_PIN "irrig1234"
#endif

static bool staConfigured()
{
    return config.network.wifi_enabled && config.network.wifi_ssid[0] != '\0';
}

static void bringUp()
{
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "Irrigacao-%s", owner.short_name);
    WiFi.mode(staConfigured() ? WIFI_AP_STA : WIFI_AP); // AP_STA mantém o STA da base vivo
    WiFi.softAP(ssid, IRRIGATION_PORTAL_PIN);
    sDns.start(53, "*", WiFi.softAPIP()); // DNS cativo: resolve tudo para o device
    LOG_INFO("Irrigation portal: AP up (%s), mode=%s", ssid, staConfigured() ? "AP_STA" : "AP");
    sApUp = true;
}

static void tearDown()
{
    sDns.stop();
    WiFi.softAPdisconnect(true);
    if (staConfigured())
        WiFi.mode(WIFI_STA); // volta a STA puro; NUNCA WIFI_OFF (mantém a LAN)
    LOG_INFO("Irrigation portal: AP down");
    sApUp = false;
}

void portalApLoop(unsigned long nowMs)
{
    if (!irrigationModule)
        return;
    PortalSession &s = irrigationModule->portalSession();
    // Renova atividade enquanto houver estação Wi-Fi associada ao AP.
    s.noteClient(nowMs, sApUp && WiFi.softAPgetStationNum() > 0);
    s.tick(nowMs);

    bool want = s.apShouldBeUp();
    if (want && !sApUp)
        bringUp();
    else if (!want && sApUp)
        tearDown();
    if (sApUp)
        sDns.processNextRequest();
}

#endif
