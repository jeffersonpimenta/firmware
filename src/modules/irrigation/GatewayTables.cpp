#include "GatewayTables.h"
#include <string.h>

namespace
{
// serialização compartilhada: magic(4)+ver(1)+count(1)+entries
size_t writeHeader(uint8_t *buf, size_t cap, uint32_t magic, uint8_t count, size_t entrySize)
{
    size_t need = 6 + (size_t)count * entrySize;
    if (cap < need)
        return 0;
    memcpy(buf, &magic, 4);
    buf[4] = 1;
    buf[5] = count;
    return need;
}
bool checkHeader(const uint8_t *buf, size_t n, uint32_t magic, size_t entrySize, uint8_t maxCount, uint8_t &countOut)
{
    if (n < 6)
        return false;
    uint32_t m;
    memcpy(&m, buf, 4);
    if (m != magic || buf[4] != 1)
        return false;
    countOut = buf[5];
    return countOut <= maxCount && n == 6 + (size_t)countOut * entrySize;
}
} // namespace

// ---- ZoneTable ----
static constexpr size_t ZONE_ENTRY = 28;

bool ZoneTable::upsert(const Zone &z)
{
    if (z.id == 0)
        return false;
    for (auto &s : zones)
        if (s.id == z.id) {
            s = z;
            return true;
        }
    for (auto &s : zones)
        if (s.id == 0) {
            s = z;
            return true;
        }
    return false;
}

bool ZoneTable::removeById(uint8_t id)
{
    for (auto &s : zones)
        if (s.id == id && id != 0) {
            s = Zone{};
            return true;
        }
    return false;
}

const Zone *ZoneTable::byId(uint8_t id) const
{
    if (id == 0)
        return nullptr;
    for (auto &s : zones)
        if (s.id == id)
            return &s;
    return nullptr;
}

const Zone *ZoneTable::byFonte(int8_t input) const
{
    if (input < 0)
        return nullptr;
    for (auto &s : zones)
        if (s.id != 0 && s.fonteInput == input)
            return &s;
    return nullptr;
}

size_t ZoneTable::count() const
{
    size_t c = 0;
    for (auto &s : zones)
        if (s.id != 0)
            c++;
    return c;
}

size_t ZoneTable::serialize(uint8_t *buf, size_t cap) const
{
    uint8_t cnt = (uint8_t)count();
    size_t need = writeHeader(buf, cap, MAGIC, cnt, ZONE_ENTRY);
    if (!need)
        return 0;
    size_t off = 6;
    for (auto &z : zones) {
        if (z.id == 0)
            continue;
        buf[off] = z.id;
        memcpy(buf + off + 1, z.name, 16);
        memcpy(buf + off + 17, &z.node, 4);
        buf[off + 21] = z.tipo;
        buf[off + 22] = z.index;
        memcpy(buf + off + 23, &z.maxMin, 2);
        memcpy(buf + off + 25, &z.padraoMin, 2);
        buf[off + 27] = (uint8_t)z.fonteInput;
        off += ZONE_ENTRY;
    }
    return need;
}

bool ZoneTable::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : zones)
        s = Zone{};
    uint8_t cnt;
    if (!checkHeader(buf, n, MAGIC, ZONE_ENTRY, MAX, cnt))
        return false;
    size_t off = 6;
    for (uint8_t i = 0; i < cnt; i++, off += ZONE_ENTRY) {
        Zone z;
        z.id = buf[off];
        memcpy(z.name, buf + off + 1, 16);
        z.name[15] = '\0';
        memcpy(&z.node, buf + off + 17, 4);
        z.tipo = buf[off + 21];
        z.index = buf[off + 22];
        memcpy(&z.maxMin, buf + off + 23, 2);
        memcpy(&z.padraoMin, buf + off + 25, 2);
        z.fonteInput = (int8_t)buf[off + 27];
        zones[i] = z;
    }
    return true;
}

// ---- StationRegistry ----
static constexpr size_t STATION_ENTRY = 87;

bool StationRegistry::upsert(const StationEntry &e)
{
    if (e.node == 0)
        return false;
    for (auto &s : stations)
        if (s.node == e.node) {
            s = e;
            return true;
        }
    for (auto &s : stations)
        if (s.node == 0) {
            s = e;
            return true;
        }
    return false;
}

bool StationRegistry::removeByNode(uint32_t node)
{
    for (auto &s : stations)
        if (s.node == node && node != 0) {
            s = StationEntry{};
            return true;
        }
    return false;
}

const StationEntry *StationRegistry::byNode(uint32_t node) const
{
    if (node == 0)
        return nullptr;
    for (auto &s : stations)
        if (s.node == node)
            return &s;
    return nullptr;
}

StationEntry *StationRegistry::mutableByNode(uint32_t node)
{
    if (node == 0)
        return nullptr;
    for (auto &s : stations)
        if (s.node == node)
            return &s;
    return nullptr;
}

size_t StationRegistry::count() const
{
    size_t c = 0;
    for (auto &s : stations)
        if (s.node != 0)
            c++;
    return c;
}

void StationRegistry::adoptConfig(uint32_t node, const uint8_t *blob52, uint32_t epoch)
{
    auto *e = mutableByNode(node);
    if (!e)
        return; // no-op se nó ausente
    if (epoch > e->desiredEpoch) {
        memcpy(e->blob, blob52, 52);
        e->desiredEpoch = epoch;
    }
}

size_t StationRegistry::serialize(uint8_t *buf, size_t cap) const
{
    uint8_t cnt = (uint8_t)count();
    size_t need = writeHeader(buf, cap, MAGIC, cnt, STATION_ENTRY);
    if (!need)
        return 0;
    size_t off = 6;
    for (auto &e : stations) {
        if (e.node == 0)
            continue;
        memcpy(buf + off, &e.node, 4);
        memcpy(buf + off + 4, e.name, 16);
        memcpy(buf + off + 20, &e.desiredEpoch, 4);
        memcpy(buf + off + 24, e.blob, 52);
        buf[off + 76] = e.retries;
        memcpy(buf + off + 77, &e.silencioAlertaMin, 2);
        memcpy(buf + off + 79, &e.lat, 4);
        memcpy(buf + off + 83, &e.lon, 4);
        off += STATION_ENTRY;
    }
    return need;
}

bool StationRegistry::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : stations)
        s = StationEntry{};
    uint8_t cnt;
    if (!checkHeader(buf, n, MAGIC, STATION_ENTRY, MAX, cnt))
        return false;
    size_t off = 6;
    for (uint8_t i = 0; i < cnt; i++, off += STATION_ENTRY) {
        StationEntry e;
        memcpy(&e.node, buf + off, 4);
        memcpy(e.name, buf + off + 4, 16);
        e.name[15] = '\0';
        memcpy(&e.desiredEpoch, buf + off + 20, 4);
        memcpy(e.blob, buf + off + 24, 52);
        e.retries = buf[off + 76];
        memcpy(&e.silencioAlertaMin, buf + off + 77, 2);
        memcpy(&e.lat, buf + off + 79, 4);
        memcpy(&e.lon, buf + off + 83, 4);
        stations[i] = e;
    }
    return true;
}
