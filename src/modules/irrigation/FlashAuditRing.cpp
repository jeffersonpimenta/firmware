#include "modules/irrigation/FlashAuditRing.h"
#include "modules/irrigation/IrrigationProtocol.h" // crc32
#include <string.h>

bool FlashAuditRing::begin() {
    if (capacity() == 0) { head = num = 0; return false; }
    uint8_t hdr[HEADER];
    if (!store.read(0, hdr, HEADER)) { clear(); return true; }
    uint32_t magic, h, c, crc;
    memcpy(&magic, hdr, 4); memcpy(&h, hdr + 4, 4); memcpy(&c, hdr + 8, 4); memcpy(&crc, hdr + 12, 4);
    if (magic != MAGIC || crc != IrrigationProto::crc32(hdr, 12) || c > capacity() || h >= capacity()) {
        clear(); return true; // formata
    }
    head = h; num = c; return true;
}
void FlashAuditRing::writeHeader() {
    uint8_t hdr[HEADER];
    uint32_t magic = MAGIC;
    memcpy(hdr, &magic, 4); memcpy(hdr + 4, &head, 4); memcpy(hdr + 8, &num, 4);
    uint32_t crc = IrrigationProto::crc32(hdr, 12); memcpy(hdr + 12, &crc, 4);
    store.write(0, hdr, HEADER);
}
void FlashAuditRing::clear() {
    head = num = 0; writeHeader();
}
void FlashAuditRing::append(const AuditRecord &r) {
    if (capacity() == 0) return;
    store.write(slotOffset(head), &r, REC);
    head = (head + 1) % capacity();
    if (num < capacity()) num++;
    writeHeader();
}
bool FlashAuditRing::at(size_t i, AuditRecord &out) const {
    if (i >= num) return false;
    // i=0 = mais recente = head-1
    uint32_t idx = (head + capacity() - 1 - (uint32_t)i) % capacity();
    return store.read(slotOffset(idx), &out, REC);
}
