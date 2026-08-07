#include "modules/irrigation/WeatherRuleTable.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include <string.h>

bool WeatherRule::coversZone(uint8_t zoneId) const {
    if (!zoneId) return false;
    for (uint8_t z : zonaIds) if (z == zoneId) return true;
    return false;
}
bool WeatherRule::coversGroup(uint8_t groupId) const {
    if (!groupId) return false;
    for (uint8_t g : grupoIds) if (g == groupId) return true;
    return false;
}

bool WeatherRuleTable::upsert(const WeatherRule &r) {
    if (!r.id) return false;
    for (auto &e : rules) if (e.id == r.id) { e = r; return true; }
    for (auto &e : rules) if (!e.id) { e = r; return true; }
    return false;
}
bool WeatherRuleTable::removeById(uint8_t id) {
    for (auto &e : rules) if (e.id == id) { e = WeatherRule{}; return true; }
    return false;
}
const WeatherRule *WeatherRuleTable::byId(uint8_t id) const {
    for (auto &e : rules) if (e.id == id) return &e;
    return nullptr;
}
const WeatherRule *WeatherRuleTable::ruleAt(size_t index) const {
    size_t k = 0;
    for (auto &e : rules) if (e.id) { if (k == index) return &e; k++; }
    return nullptr;
}
size_t WeatherRuleTable::count() const {
    size_t k = 0; for (auto &e : rules) if (e.id) k++; return k;
}
uint8_t WeatherRuleTable::nextFreeId() const {
    for (uint16_t id = 1; id <= 255; id++) if (!byId((uint8_t)id)) return (uint8_t)id;
    return 0;
}

size_t WeatherRuleTable::serialize(uint8_t *buf, size_t cap) const {
    size_t n = count();
    size_t need = 4 + 2 + n * sizeof(WeatherRule) + 4;
    if (cap < need) return 0;
    size_t o = 0;
    uint32_t magic = MAGIC; memcpy(buf + o, &magic, 4); o += 4;
    uint16_t c = (uint16_t)n; memcpy(buf + o, &c, 2); o += 2;
    for (auto &e : rules) if (e.id) { memcpy(buf + o, &e, sizeof(e)); o += sizeof(e); }
    uint32_t crc = IrrigationProto::crc32(buf, o); memcpy(buf + o, &crc, 4); o += 4;
    return o;
}
bool WeatherRuleTable::deserialize(const uint8_t *buf, size_t n) {
    for (auto &e : rules) e = WeatherRule{};
    if (n < 10) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != MAGIC) return false;
    uint16_t c; memcpy(&c, buf + 4, 2);
    size_t need = 4 + 2 + (size_t)c * sizeof(WeatherRule) + 4;
    if (n != need || c > MAX) return false;
    uint32_t crc; memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4)) return false;
    size_t o = 6;
    for (uint16_t i = 0; i < c; i++) { memcpy(&rules[i], buf + o, sizeof(WeatherRule)); o += sizeof(WeatherRule); }
    return true;
}
