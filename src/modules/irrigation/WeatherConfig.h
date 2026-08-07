#pragma once
#include <stddef.h>
#include <stdint.h>

// Config compartilhada do subsistema de clima (persistida gateway-side).
// Layout serializado: magic(4) + version(2) + payload(14) + crc32(4) = 24 bytes.
struct WeatherConfig {
    static constexpr uint32_t MAGIC = 0x57544831; // "WTH1"
    static constexpr uint16_t VERSION = 1;

    uint8_t  enabled = 0;      // master on/off
    int32_t  latE7 = 0;        // latitude ×1e7
    int32_t  lonE7 = 0;        // longitude ×1e7
    uint8_t  pollHourA = 4;    // 1º poll do dia (hora local 0..23)
    uint8_t  pollHourB = 16;   // 2º poll do dia
    uint16_t staleTtlH = 24;   // TTL do cache p/ fail-open (horas)

    // magic+version+enabled+lat+lon+pollA+pollB+ttl+crc
    static constexpr size_t SERIALIZED = 4 + 2 + 1 + 4 + 4 + 1 + 1 + 2 + 4; // 23

    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // false => mantém defaults
};
