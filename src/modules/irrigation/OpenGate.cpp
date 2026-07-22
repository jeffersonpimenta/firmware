#include "modules/irrigation/OpenGate.h"

size_t OpenGate::openCount() const {
    size_t c = 0;
    for (uint8_t z : open) if (z) c++;
    return c;
}
bool OpenGate::markOpen(uint8_t zoneId) {
    for (uint8_t z : open) if (z == zoneId) return true; // já aberta
    for (uint8_t &z : open) if (!z) { z = zoneId; return true; }
    return false; // sem slot (não deveria: MAX_OPEN = ZoneTable::MAX)
}
bool OpenGate::isQueued(uint8_t zoneId) const {
    for (size_t i = 0; i < qSize; i++) if (queue[(qHead + i) % MAX_QUEUE].zoneId == zoneId) return true;
    return false;
}
OpenGate::Decision OpenGate::request(uint8_t zoneId, uint16_t durationS) {
    for (uint8_t z : open) if (z == zoneId) return Decision::ADMIT; // renovação idempotente
    if (hasCapacity()) { markOpen(zoneId); return Decision::ADMIT; }
    if (!isQueued(zoneId) && qSize < MAX_QUEUE) {
        queue[qTail] = { zoneId, durationS }; qTail = (qTail + 1) % MAX_QUEUE; qSize++;
    }
    return Decision::HOLD;
}
void OpenGate::release(uint8_t zoneId) {
    for (uint8_t &z : open) if (z == zoneId) { z = 0; return; }
}
OpenGate::Pending OpenGate::nextAdmittable() {
    if (qSize == 0 || !hasCapacity()) return {};
    Pending p = queue[qHead]; qHead = (qHead + 1) % MAX_QUEUE; qSize--;
    return p; // glue chama request(p.zoneId, p.durationS) em seguida p/ registrar a abertura
}
