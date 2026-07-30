#include "modules/irrigation/LevelControlTable.h"
#include "modules/irrigation/IrrigationProtocol.h" // crc32
#include <string.h>

size_t LevelControlTable::count() const {
    size_t c = 0;
    for (auto &r : rules) if (r.id) c++;
    return c;
}

const LevelRule *LevelControlTable::byId(uint8_t id) const {
    if (!id) return nullptr;
    for (auto &r : rules) if (r.id == id) return &r;
    return nullptr;
}

const LevelRule *LevelControlTable::ruleAt(size_t index) const {
    for (auto &r : rules) if (r.id) { if (index == 0) return &r; index--; }
    return nullptr;
}

bool LevelControlTable::upsert(const LevelRule &in) {
    if (!in.id) return false;
    for (auto &r : rules) if (r.id == in.id) { r = in; return true; } // atualiza
    for (auto &r : rules) if (!r.id) { r = in; return true; }         // insere
    return false; // tabela cheia
}

bool LevelControlTable::removeById(uint8_t id) {
    if (!id) return false;
    for (auto &r : rules) if (r.id == id) { r = LevelRule{}; return true; }
    return false;
}

// serialize: magic(4) + count(2) + N×sizeof(LevelRule) + crc32(4).
size_t LevelControlTable::serialize(uint8_t *buf, size_t cap) const {
    size_t n = count();
    size_t need = 4 + 2 + n * sizeof(LevelRule) + 4;
    if (cap < need) return 0;
    size_t o = 0;
    uint32_t magic = MAGIC; memcpy(buf + o, &magic, 4); o += 4;
    uint16_t c = (uint16_t)n; memcpy(buf + o, &c, 2); o += 2;
    for (auto &r : rules) if (r.id) { memcpy(buf + o, &r, sizeof(r)); o += sizeof(r); }
    uint32_t crc = IrrigationProto::crc32(buf, o); memcpy(buf + o, &crc, 4); o += 4;
    return o;
}

bool LevelControlTable::deserialize(const uint8_t *buf, size_t n) {
    for (auto &r : rules) r = LevelRule{};
    if (n < 10) return false;
    uint32_t magic; memcpy(&magic, buf, 4);
    if (magic != MAGIC) return false;
    uint16_t c; memcpy(&c, buf + 4, 2);
    size_t need = 4 + 2 + (size_t)c * sizeof(LevelRule) + 4;
    if (n != need || c > MAX) return false;
    uint32_t crc; memcpy(&crc, buf + n - 4, 4);
    if (crc != IrrigationProto::crc32(buf, n - 4)) return false;
    size_t o = 6;
    for (uint16_t i = 0; i < c; i++) { memcpy(&rules[i], buf + o, sizeof(LevelRule)); o += sizeof(LevelRule); }
    return true;
}
