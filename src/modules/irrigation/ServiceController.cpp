#include "modules/irrigation/ServiceController.h"
#include <cstring>

using namespace IrrigationService;

RetunePlan channelFromProfile(const LightProfile &p)
{
    RetunePlan r{};
    strncpy(r.name, p.canalNome, 12);
    r.name[12] = 0;
    r.preset = p.preset;
    int m = base64Decode(p.pskB64, strlen(p.pskB64), r.psk, sizeof r.psk);
    if (m < 0 || r.name[0] == 0) {
        r.ok = false;
        return r;
    }
    r.pskLen = (size_t)m;
    r.ok = true;
    return r;
}

RouteDecision decideConfigRoute(bool gatewayReachable, uint32_t currentEpoch)
{
    if (gatewayReachable)
        return {ConfigRoute::VIA_GATEWAY, 0};
    return {ConfigRoute::DIRECT, currentEpoch + 1};
}

void ScanResults::add(const ScanEntry &x)
{
    for (size_t i = 0; i < n; i++)
        if (e[i].node == x.node) {
            e[i] = x;
            return;
        }
    if (n < MAX)
        e[n++] = x;
}
