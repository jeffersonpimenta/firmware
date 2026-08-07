#include "modules/irrigation/WeatherConfig.h"
#include "modules/irrigation/IrrigationProtocol.h" // IrrigationProto::crc32
#include <string.h>

size_t WeatherConfig::serialize(uint8_t *buf, size_t cap) const {
    if (cap < SERIALIZED) return 0;
    size_t o = 0;
    uint32_t magic = MAGIC; memcpy(buf + o, &magic, 4); o += 4;
    uint16_t ver = VERSION; memcpy(buf + o, &ver, 2); o += 2;
    buf[o++] = enabled;
    memcpy(buf + o, &latE7, 4); o += 4;
    memcpy(buf + o, &lonE7, 4); o += 4;
    buf[o++] = pollHourA;
    buf[o++] = pollHourB;
    memcpy(buf + o, &staleTtlH, 2); o += 2;
    uint32_t crc = IrrigationProto::crc32(buf, o); memcpy(buf + o, &crc, 4); o += 4;
    return o;
}

bool WeatherConfig::deserialize(const uint8_t *buf, size_t n) {
    if (n != SERIALIZED) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != MAGIC) return false;
    uint32_t crc; memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4)) return false;
    size_t o = 6; // após magic+version
    WeatherConfig tmp;
    tmp.enabled = buf[o++];
    memcpy(&tmp.latE7, buf + o, 4); o += 4;
    memcpy(&tmp.lonE7, buf + o, 4); o += 4;
    tmp.pollHourA = buf[o++];
    tmp.pollHourB = buf[o++];
    memcpy(&tmp.staleTtlH, buf + o, 2); o += 2;
    *this = tmp;
    return true;
}
