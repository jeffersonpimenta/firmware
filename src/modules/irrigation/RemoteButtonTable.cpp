#include "RemoteButtonTable.h"
#include <string.h>

static constexpr size_t REMOTE_ENTRY = 3 + RemoteAssoc::MAX_TRIGGERS * 6; // id,enabled,zone + 4×(node4+in1+led1)

uint8_t RemoteAssoc::triggerCount() const
{
    uint8_t c = 0;
    for (const auto &t : triggers)
        if (t.node != 0)
            c++;
    return c;
}

bool RemoteButtonTable::upsert(const RemoteAssoc &a)
{
    if (a.id == 0)
        return false;
    for (auto &s : assocs)
        if (s.id == a.id) { s = a; return true; }
    for (auto &s : assocs)
        if (s.id == 0) { s = a; return true; }
    return false;
}

bool RemoteButtonTable::removeById(uint8_t id)
{
    for (auto &s : assocs)
        if (s.id == id && id != 0) { s = RemoteAssoc{}; return true; }
    return false;
}

const RemoteAssoc *RemoteButtonTable::byId(uint8_t id) const
{
    if (id == 0) return nullptr;
    for (const auto &s : assocs)
        if (s.id == id) return &s;
    return nullptr;
}

const RemoteAssoc *RemoteButtonTable::assocAt(size_t index) const
{
    size_t seen = 0;
    for (const auto &s : assocs)
        if (s.id != 0) { if (seen == index) return &s; seen++; }
    return nullptr;
}

size_t RemoteButtonTable::count() const
{
    size_t c = 0;
    for (const auto &s : assocs)
        if (s.id != 0) c++;
    return c;
}

size_t RemoteButtonTable::findByTrigger(uint32_t node, uint8_t inputIdx, const RemoteAssoc **out, size_t outCap) const
{
    size_t k = 0;
    for (const auto &s : assocs) {
        if (s.id == 0) continue;
        for (const auto &t : s.triggers)
            if (t.node == node && t.node != 0 && t.inputIdx == inputIdx) {
                if (k < outCap) out[k] = &s;
                k++;
                break;
            }
    }
    return k;
}

size_t RemoteButtonTable::serialize(uint8_t *buf, size_t cap) const
{
    uint8_t cnt = (uint8_t)count();
    size_t need = 6 + (size_t)cnt * REMOTE_ENTRY;
    if (cap < need) return 0;
    uint32_t magic = MAGIC;
    memcpy(buf, &magic, 4);
    buf[4] = 1;
    buf[5] = cnt;
    size_t o = 6;
    for (const auto &s : assocs) {
        if (s.id == 0) continue;
        buf[o++] = s.id;
        buf[o++] = s.enabled;
        buf[o++] = s.targetZoneId;
        for (const auto &t : s.triggers) {
            memcpy(buf + o, &t.node, 4); o += 4;
            buf[o++] = t.inputIdx;
            buf[o++] = t.ledSlot;
        }
    }
    return need;
}

bool RemoteButtonTable::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : assocs) s = RemoteAssoc{};
    if (n < 6) return false;
    uint32_t m; memcpy(&m, buf, 4);
    if (m != MAGIC || buf[4] != 1) return false;
    uint8_t cnt = buf[5];
    if (cnt > MAX || n != 6 + (size_t)cnt * REMOTE_ENTRY) return false;
    size_t o = 6;
    for (uint8_t i = 0; i < cnt; i++) {
        RemoteAssoc a;
        a.id = buf[o++]; a.enabled = buf[o++]; a.targetZoneId = buf[o++];
        for (auto &t : a.triggers) {
            memcpy(&t.node, buf + o, 4); o += 4;
            t.inputIdx = buf[o++]; t.ledSlot = buf[o++];
        }
        assocs[i] = a;
    }
    return true;
}
