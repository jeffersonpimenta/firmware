#include "modules/irrigation/WeatherEngine.h"

namespace WeatherEngine {

bool cacheFresh(const WeatherCache &cache, uint32_t nowEpoch, uint16_t ttlHours) {
    if (!cache.valid || nowEpoch == 0) return false;
    if (nowEpoch < cache.fetchEpoch) return false; // relógio andou pra trás
    return (nowEpoch - cache.fetchEpoch) <= (uint32_t)ttlHours * 3600u;
}

bool ruleTriggered(const WeatherRule &r, const WeatherCache &cache,
                   uint32_t nowEpoch, uint16_t ttlHours) {
    if (!r.enabled) return false;
    if (!cacheFresh(cache, nowEpoch, ttlHours)) return false;
    return (cache.chuvaPrevista12hCenti > r.limiarMmCenti) &&
           (cache.probChuvaPct > r.limiarPct);
}

static WeatherVerdict verdictBy(bool (WeatherRule::*covers)(uint8_t) const, uint8_t targetId,
                                const WeatherCache &cache, const WeatherRuleTable &rules,
                                uint32_t nowEpoch, uint16_t ttlHours) {
    WeatherVerdict v;
    if (!cacheFresh(cache, nowEpoch, ttlHours)) return v; // fail-open
    for (size_t i = 0; i < rules.count(); i++) {
        const WeatherRule *r = rules.ruleAt(i);
        if (!r || !r->enabled) continue;
        if (!(r->*covers)(targetId)) continue;
        if (ruleTriggered(*r, cache, nowEpoch, ttlHours)) { v.suppress = true; v.ruleId = r->id; return v; }
    }
    return v;
}

WeatherVerdict zoneVerdict(uint8_t zoneId, const WeatherCache &cache,
                           const WeatherRuleTable &rules, uint32_t nowEpoch, uint16_t ttlHours) {
    return verdictBy(&WeatherRule::coversZone, zoneId, cache, rules, nowEpoch, ttlHours);
}
WeatherVerdict groupVerdict(uint8_t groupId, const WeatherCache &cache,
                            const WeatherRuleTable &rules, uint32_t nowEpoch, uint16_t ttlHours) {
    return verdictBy(&WeatherRule::coversGroup, groupId, cache, rules, nowEpoch, ttlHours);
}

} // namespace WeatherEngine
