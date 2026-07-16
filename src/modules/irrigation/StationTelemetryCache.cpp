#include "modules/irrigation/StationTelemetryCache.h"

void StationTelemetryCache::update(const StationTelemetry &t)
{
    if (t.node == 0)
        return;
    int free = -1, oldest = 0;
    uint32_t oldestMs = 0xFFFFFFFF;
    for (size_t i = 0; i < MAX; i++) {
        if (entries[i].node == t.node) {
            entries[i] = t;
            return;
        }
        if (entries[i].node == 0 && free < 0)
            free = (int)i;
        if (entries[i].atMs < oldestMs) {
            oldestMs = entries[i].atMs;
            oldest = (int)i;
        }
    }
    entries[(free >= 0) ? (size_t)free : (size_t)oldest] = t; // recicla o mais antigo se cheio
}

const StationTelemetry *StationTelemetryCache::byNode(uint32_t node) const
{
    for (size_t i = 0; i < MAX; i++)
        if (entries[i].node == node)
            return &entries[i];
    return nullptr;
}
