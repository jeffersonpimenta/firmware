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

bool ServiceController::planRetune(const char *id, RetunePlan &out)
{
    LightProfile lp[16];
    size_t k = vault.listClients(lp, 16);
    for (size_t i = 0; i < k; i++)
        if (strcmp(lp[i].id, id) == 0) {
            out = channelFromProfile(lp[i]);
            return out.ok;
        }
    return false;
}

uint32_t ServiceController::nextSeq(const char *id, uint32_t node, bool &needResync)
{
    needResync = needsResync(vault.hasSeq(id, node));
    if (needResync)
        return 0;
    uint32_t s = vault.seqFor(id, node) + 1;
    vault.setSeq(id, node, s);
    return s;
}

void ServiceController::onResyncReply(const char *id, uint32_t node, uint32_t lastSeq)
{
    // Store the receiver's last-seen seq; the next nextSeq() then yields
    // resumeSeqFrom(lastSeq) == lastSeq+1, i.e. resume outgoing at lastSeq+1 (§11.5).
    vault.setSeq(id, node, lastSeq);
}
