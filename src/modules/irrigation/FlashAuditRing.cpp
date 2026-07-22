#include "modules/irrigation/FlashAuditRing.h"
#include "modules/irrigation/IrrigationProtocol.h" // crc32
#include <stdio.h>
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
size_t FlashAuditRing::toCsv(char *buf, size_t cap, size_t maxRecords) const {
    size_t o = 0;
    int w = snprintf(buf + o, cap - o, "ts,origem,acao,alvo,resultado,node,seq\n");
    if (w < 0 || (size_t)w >= cap - o) return o; o += w;
    size_t lim = num < maxRecords ? num : maxRecords;
    for (size_t i = 0; i < lim; i++) {
        AuditRecord r; if (!at(i, r)) break;
        char line[96];
        int lw = snprintf(line, sizeof(line), "%u,%u,%u,%u,%u,%08x,%u\n",
                          (unsigned)r.tsSecs, r.origin, r.action, r.target, r.result,
                          (unsigned)r.node, (unsigned)r.seq);
        if (lw < 0 || o + (size_t)lw >= cap) break; // trunca no que couber inteiro
        memcpy(buf + o, line, lw); o += lw;
    }
    return o;
}
size_t FlashAuditRing::toJson(char *buf, size_t cap, size_t maxRecords) const {
    size_t o = 0;
    if (cap < 2) return 0;
    buf[o++] = '[';
    size_t lim = num < maxRecords ? num : maxRecords;
    for (size_t i = 0; i < lim; i++) {
        AuditRecord r; if (!at(i, r)) break;
        char item[128];
        int lw = snprintf(item, sizeof(item),
            "%s{\"ts\":%u,\"origem\":%u,\"acao\":%u,\"alvo\":%u,\"resultado\":%u,\"node\":%u,\"seq\":%u}",
            i ? "," : "", (unsigned)r.tsSecs, r.origin, r.action, r.target, r.result,
            (unsigned)r.node, (unsigned)r.seq);
        if (lw < 0 || o + (size_t)lw + 1 >= cap) break; // +1 p/ o ']'
        memcpy(buf + o, item, lw); o += lw;
    }
    buf[o++] = ']';
    return o;
}
