#include "modules/irrigation/PortalAp.h"
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WEBSERVER

#include "configuration.h"
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

static void bringUp()
{
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "Irrigacao-%s", owner.short_name);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, IRRIGATION_PORTAL_PIN);
    sDns.start(53, "*", WiFi.softAPIP()); // DNS cativo: resolve tudo para o device
    LOG_INFO("Irrigation portal: AP up (%s)", ssid);
    sApUp = true;
}

static void tearDown()
{
    sDns.stop();
    WiFi.softAPdisconnect(true);
    LOG_INFO("Irrigation portal: AP down");
    sApUp = false;
}

void portalApLoop(uint32_t nowMs)
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
