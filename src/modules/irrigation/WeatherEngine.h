#pragma once
#include "modules/irrigation/WeatherForecast.h"
#include "modules/irrigation/WeatherRuleTable.h"
#include <stdint.h>

namespace WeatherEngine {

// TTL/fail-open: true só se cache válido e não expirado (e nowEpoch>0).
bool cacheFresh(const WeatherCache &cache, uint32_t nowEpoch, uint16_t ttlHours);

// Suprime se alguma regra habilitada que cobre o alvo satisfaz
// (chuva12h > limiarMm) E (prob > limiarPct). Fail-open se cache não-fresco.
WeatherVerdict zoneVerdict(uint8_t zoneId, const WeatherCache &cache,
                           const WeatherRuleTable &rules, uint32_t nowEpoch, uint16_t ttlHours);
WeatherVerdict groupVerdict(uint8_t groupId, const WeatherCache &cache,
                            const WeatherRuleTable &rules, uint32_t nowEpoch, uint16_t ttlHours);

// true se a regra dispara AGORA contra o cache (ignora cobertura de alvo).
// Usado pela UI para o badge "Suprimindo" e "triggered".
bool ruleTriggered(const WeatherRule &r, const WeatherCache &cache,
                   uint32_t nowEpoch, uint16_t ttlHours);

} // namespace WeatherEngine
