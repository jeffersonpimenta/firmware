#include "Allowlist.h"
#include <string.h>

bool Allowlist::add(uint32_t nodeId)
{
    if (contains(nodeId))
        return true;
    if (n >= MAX)
        return false;
    ids[n++] = nodeId;
    return true;
}

bool Allowlist::remove(uint32_t nodeId)
{
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == nodeId) {
            ids[i] = ids[--n];
            return true;
        }
    }
    return false;
}

bool Allowlist::contains(uint32_t nodeId) const
{
    for (size_t i = 0; i < n; i++)
        if (ids[i] == nodeId)
            return true;
    return false;
}

size_t Allowlist::serialize(uint8_t *buf, size_t cap) const
{
    size_t need = 4 + 1 + 1 + n * 4;
    if (cap < need)
        return 0;
    uint32_t m = MAGIC;
    memcpy(buf, &m, 4);
    buf[4] = 1; // versão
    buf[5] = (uint8_t)n;
    for (size_t i = 0; i < n; i++)
        memcpy(buf + 6 + i * 4, &ids[i], 4);
    return need;
}

bool Allowlist::deserialize(const uint8_t *buf, size_t len)
{
    n = 0;
    if (len < 6)
        return false;
    uint32_t magic;
    memcpy(&magic, buf, 4);
    if (magic != MAGIC || buf[4] != 1)
        return false;
    size_t cnt = buf[5];
    if (cnt > MAX || len != 6 + cnt * 4)
        return false;
    for (size_t i = 0; i < cnt; i++)
        memcpy(&ids[i], buf + 6 + i * 4, 4);
    n = cnt;
    return true;
}
