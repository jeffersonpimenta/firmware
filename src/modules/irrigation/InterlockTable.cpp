#include "modules/irrigation/InterlockTable.h"
#include "modules/irrigation/IrrigationProtocol.h" // crc32
#include <string.h>

size_t InterlockTable::count() const {
    size_t c = 0;
    for (auto &r : rules) if (r.id) c++;
    return c;
}

const InterlockRule *InterlockTable::byId(uint8_t id) const {
    if (!id) return nullptr;
    for (auto &r : rules) if (r.id == id) return &r;
    return nullptr;
}

const InterlockRule *InterlockTable::ruleAt(size_t index) const {
    for (auto &r : rules) if (r.id) { if (index == 0) return &r; index--; }
    return nullptr;
}

const InterlockRule *InterlockTable::ruleAtSlot(size_t slot) const {
    if (slot >= MAX) return nullptr;
    return rules[slot].id ? &rules[slot] : nullptr;
}

bool InterlockTable::upsert(const InterlockRule &in) {
    if (!in.id) return false;
    for (auto &r : rules) if (r.id == in.id) { r = in; return true; } // atualiza
    for (auto &r : rules) if (!r.id) { r = in; return true; }         // insere
    return false; // tabela cheia
}

bool InterlockTable::removeById(uint8_t id) {
    if (!id) return false;
    for (auto &r : rules) if (r.id == id) { r = InterlockRule{}; return true; }
    return false;
}

// serialize: magic(4) + count(2) + N×sizeof(InterlockRule) + crc32(4).
size_t InterlockTable::serialize(uint8_t *buf, size_t cap) const {
    size_t n = count();
    size_t need = 4 + 2 + n * sizeof(InterlockRule) + 4;
    if (cap < need) return 0;
    size_t o = 0;
    uint32_t magic = MAGIC; memcpy(buf + o, &magic, 4); o += 4;
    uint16_t c = (uint16_t)n; memcpy(buf + o, &c, 2); o += 2;
    for (auto &r : rules) if (r.id) { memcpy(buf + o, &r, sizeof(r)); o += sizeof(r); }
    uint32_t crc = IrrigationProto::crc32(buf, o); memcpy(buf + o, &crc, 4); o += 4;
    return o;
}

bool InterlockTable::deserialize(const uint8_t *buf, size_t n) {
    for (auto &r : rules) r = InterlockRule{};
    if (n < 10) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != MAGIC) return false;
    uint16_t c; memcpy(&c, buf + 4, 2);
    size_t need = 4 + 2 + (size_t)c * sizeof(InterlockRule) + 4;
    if (n != need || c > MAX) return false;
    uint32_t crc; memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4)) return false;
    size_t o = 6;
    for (uint16_t i = 0; i < c; i++) { memcpy(&rules[i], buf + o, sizeof(InterlockRule)); o += sizeof(InterlockRule); }
    return true;
}
