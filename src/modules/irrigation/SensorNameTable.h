#pragma once
#include <stddef.h>
#include <stdint.h>

struct SensorName {
    uint32_t node = 0;      // 0 = slot vazio
    uint8_t sensorIdx = 0;
    char name[16] = {0};
};

class SensorNameTable {
  public:
    static constexpr size_t MAX = 32;
    static constexpr uint32_t MAGIC = 0x49534E31; // "ISN1"
    bool set(uint32_t node, uint8_t sensorIdx, const char *name); // upsert; false = cheia
    const char *get(uint32_t node, uint8_t sensorIdx) const;      // nullptr = sem nome
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);

  private:
    SensorName names[MAX];
};
