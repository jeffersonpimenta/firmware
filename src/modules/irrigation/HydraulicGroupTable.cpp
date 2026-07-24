#include "HydraulicGroupTable.h"
#include <string.h>

namespace
{
// Entrada fixa: id(1)+name(16)+bomba(1)+zoneIds(8)+zoneCount(1)+minOpen(1)+maxOpen(1)
//   +transicao(1)+overlapS(2)+startAfterOpenS(2)+stopBeforeCloseS(2)+minRunMin(2)+maxStartsHour(1) = 39 B
constexpr size_t ENTRY = 1 + 16 + 1 + 8 + 1 + 1 + 1 + 1 + 2 + 2 + 2 + 2 + 1;

uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}
} // namespace

bool HydraulicGroupTable::zoneUsedByOther(uint8_t zoneId, uint8_t exceptId) const
{
    for (const auto &g : groups) {
        if (g.id == 0 || g.id == exceptId)
            continue;
        for (uint8_t i = 0; i < g.zoneCount; i++)
            if (g.zoneIds[i] == zoneId)
                return true;
    }
    return false;
}

bool HydraulicGroupTable::valid(const HydraulicGroup &g) const
{
    if (g.id == 0 || g.zoneCount == 0 || g.zoneCount > 8)
        return false;
    if (g.minOpen == 0 || g.minOpen > g.zoneCount)
        return false;
    if (g.maxOpen != 0 && g.maxOpen < g.minOpen)
        return false;
    for (uint8_t i = 0; i < g.zoneCount; i++) {
        uint8_t z = g.zoneIds[i];
        if (z == 0 || z == g.bombaZoneId)   // membro nulo ou == bomba
            return false;
        for (uint8_t j = i + 1; j < g.zoneCount; j++)
            if (g.zoneIds[j] == z)           // duplicata interna
                return false;
        if (zoneUsedByOther(z, g.id))
            return false;
    }
    return true;
}

bool HydraulicGroupTable::upsert(const HydraulicGroup &g)
{
    if (!valid(g))
        return false;
    for (auto &s : groups)
        if (s.id == g.id) {
            s = g;
            return true;
        }
    for (auto &s : groups)
        if (s.id == 0) {
            s = g;
            return true;
        }
    return false; // cheia
}

bool HydraulicGroupTable::removeById(uint8_t id)
{
    if (id == 0)
        return false;
    for (auto &s : groups)
        if (s.id == id) {
            s = HydraulicGroup{};
            return true;
        }
    return false;
}

const HydraulicGroup *HydraulicGroupTable::byId(uint8_t id) const
{
    if (id == 0)
        return nullptr;
    for (const auto &s : groups)
        if (s.id == id)
            return &s;
    return nullptr;
}

const HydraulicGroup *HydraulicGroupTable::byZone(uint8_t zoneId) const
{
    if (zoneId == 0)
        return nullptr;
    for (const auto &s : groups) {
        if (s.id == 0)
            continue;
        for (uint8_t i = 0; i < s.zoneCount; i++)
            if (s.zoneIds[i] == zoneId)
                return &s;
    }
    return nullptr;
}

const HydraulicGroup *HydraulicGroupTable::byPumpZone(uint8_t zoneId) const
{
    if (zoneId == 0)
        return nullptr;
    for (const auto &s : groups)
        if (s.id != 0 && s.bombaZoneId == zoneId)
            return &s;
    return nullptr;
}

const HydraulicGroup *HydraulicGroupTable::groupAt(size_t index) const
{
    size_t seen = 0;
    for (const auto &s : groups) {
        if (s.id == 0)
            continue;
        if (seen == index)
            return &s;
        seen++;
    }
    return nullptr;
}

size_t HydraulicGroupTable::count() const
{
    size_t c = 0;
    for (const auto &s : groups)
        if (s.id != 0)
            c++;
    return c;
}

size_t HydraulicGroupTable::serialize(uint8_t *buf, size_t cap) const
{
    uint8_t cnt = (uint8_t)count();
    size_t need = 6 + (size_t)cnt * ENTRY + 4; // header(6) + entries + crc(4)
    if (cap < need)
        return 0;
    memcpy(buf, &MAGIC, 4);
    buf[4] = 1; // versão
    buf[5] = cnt;
    size_t off = 6;
    for (const auto &g : groups) {
        if (g.id == 0)
            continue;
        buf[off] = g.id;
        memcpy(buf + off + 1, g.name, 16);
        buf[off + 17] = g.bombaZoneId;
        memcpy(buf + off + 18, g.zoneIds, 8);
        buf[off + 26] = g.zoneCount;
        buf[off + 27] = g.minOpen;
        buf[off + 28] = g.maxOpen;
        buf[off + 29] = g.transicao;
        memcpy(buf + off + 30, &g.overlapS, 2);
        memcpy(buf + off + 32, &g.startAfterOpenS, 2);
        memcpy(buf + off + 34, &g.stopBeforeCloseS, 2);
        memcpy(buf + off + 36, &g.minRunMin, 2);
        buf[off + 38] = g.maxStartsHour;
        off += ENTRY;
    }
    uint32_t c = crc32(buf, off);
    memcpy(buf + off, &c, 4);
    return off + 4;
}

bool HydraulicGroupTable::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : groups)
        s = HydraulicGroup{};
    if (n < 10)
        return false;
    uint32_t m;
    memcpy(&m, buf, 4);
    if (m != MAGIC || buf[4] != 1)
        return false;
    uint8_t cnt = buf[5];
    if (cnt > MAX)
        return false;
    size_t need = 6 + (size_t)cnt * ENTRY + 4;
    if (n != need)
        return false;
    uint32_t stored;
    memcpy(&stored, buf + need - 4, 4);
    if (crc32(buf, need - 4) != stored)
        return false;
    size_t off = 6;
    for (uint8_t i = 0; i < cnt; i++, off += ENTRY) {
        HydraulicGroup &g = groups[i];
        g.id = buf[off];
        memcpy(g.name, buf + off + 1, 16);
        g.name[15] = 0;
        g.bombaZoneId = buf[off + 17];
        memcpy(g.zoneIds, buf + off + 18, 8);
        g.zoneCount = buf[off + 26];
        g.minOpen = buf[off + 27];
        g.maxOpen = buf[off + 28];
        g.transicao = buf[off + 29];
        memcpy(&g.overlapS, buf + off + 30, 2);
        memcpy(&g.startAfterOpenS, buf + off + 32, 2);
        memcpy(&g.stopBeforeCloseS, buf + off + 34, 2);
        memcpy(&g.minRunMin, buf + off + 36, 2);
        g.maxStartsHour = buf[off + 38];
        if (g.zoneCount > 8) { // guarda contra blob adulterado
            for (auto &s : groups)
                s = HydraulicGroup{};
            return false;
        }
    }
    return true;
}
