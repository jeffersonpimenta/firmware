#pragma once
#include <stddef.h>
#include <stdint.h>

// Allowlist de estações adotadas pelo gateway (spec §6). Persistência = módulo.
class Allowlist
{
  public:
    static constexpr size_t MAX = 16;
    static constexpr uint32_t MAGIC = 0x49414C31; // "IAL1"

    bool add(uint32_t nodeId);
    bool remove(uint32_t nodeId);
    bool contains(uint32_t nodeId) const;
    size_t count() const { return n; }
    // Retorna o nodeId na posição i (0-based). 0 se fora do range.
    uint32_t nodeAt(size_t i) const { return i < n ? ids[i] : 0; }
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t len);

  private:
    uint32_t ids[MAX] = {};
    size_t n = 0;
};
