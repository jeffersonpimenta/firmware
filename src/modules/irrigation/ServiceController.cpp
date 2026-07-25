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
