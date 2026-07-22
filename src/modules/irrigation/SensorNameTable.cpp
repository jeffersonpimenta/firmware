#include "modules/irrigation/SensorNameTable.h"
#include "modules/irrigation/IrrigationProtocol.h" // crc32
#include <string.h>

size_t SensorNameTable::count() const {
    size_t c = 0;
    for (auto &e : names) if (e.node) c++;
    return c;
}

const char *SensorNameTable::get(uint32_t node, uint8_t sensorIdx) const {
    if (!node) return nullptr;
    for (auto &e : names) if (e.node == node && e.sensorIdx == sensorIdx) return e.name;
    return nullptr;
}

bool SensorNameTable::set(uint32_t node, uint8_t sensorIdx, const char *name) {
    if (!node) return false;
    auto store = [&](SensorName &e) {
        e.node = node; e.sensorIdx = sensorIdx;
        strncpy(e.name, name ? name : "", sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
    };
    for (auto &e : names) if (e.node == node && e.sensorIdx == sensorIdx) { store(e); return true; } // update
    for (auto &e : names) if (!e.node) { store(e); return true; }                                    // insert
    return false; // cheia
}

// serialize: magic(4) + count(2) + N×sizeof(SensorName) + crc32(4).
size_t SensorNameTable::serialize(uint8_t *buf, size_t cap) const {
    size_t need = 4 + 2 + count() * sizeof(SensorName) + 4;
    if (cap < need) return 0;
    size_t o = 0;
    uint32_t magic = MAGIC; memcpy(buf + o, &magic, 4); o += 4;
    uint16_t c = (uint16_t)count(); memcpy(buf + o, &c, 2); o += 2;
    for (auto &e : names) if (e.node) { memcpy(buf + o, &e, sizeof(e)); o += sizeof(e); }
    uint32_t crc = IrrigationProto::crc32(buf, o); memcpy(buf + o, &crc, 4); o += 4;
    return o;
}

bool SensorNameTable::deserialize(const uint8_t *buf, size_t n) {
    for (auto &e : names) e = SensorName{};
    if (n < 10) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != MAGIC) return false;
    uint16_t c; memcpy(&c, buf + 4, 2);
    size_t need = 4 + 2 + (size_t)c * sizeof(SensorName) + 4;
    if (n != need || c > MAX) return false;
    uint32_t crc; memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4)) return false;
    size_t o = 6;
    for (uint16_t i = 0; i < c; i++) { memcpy(&names[i], buf + o, sizeof(SensorName)); o += sizeof(SensorName); }
    return true;
}
