#include "modules/irrigation/ServiceVault.h"
#include "modules/irrigation/IrrigationWebApi.h" // JsonWriter
#include <cstring>

using namespace IrrigationService;

struct ImportCtx {
    IProfileStore *store;
    char err[48];
    bool ok;
};
static bool importClientCb(void *c, Slice cl)
{
    auto *ic = (ImportCtx *)c;
    char id[32];
    if (!jsonStr(cl, "id", id, sizeof id) || id[0] == 0) {
        ic->ok = false;
        strcpy(ic->err, "client missing id");
        return false;
    }
    if (!ic->store->writeProfile(id, cl.p, cl.n)) {
        ic->ok = false;
        strcpy(ic->err, "store write failed");
        return false;
    }
    return true;
}

bool ServiceVault::importEnvelope(const char *json, size_t n, bool replace, char *err, size_t errCap)
{
    if (!validateEnvelope(json, n, err, errCap))
        return false;
    if (replace) {
        char ids[16][32];
        size_t k = store.listIds(ids, 16);
        for (size_t i = 0; i < k; i++)
            store.removeProfile(ids[i]);
    }
    ImportCtx ic{&store, "", true};
    envelopeForEachClient(json, n, &ic, importClientCb);
    if (!ic.ok && errCap) {
        strncpy(err, ic.err, errCap - 1);
        err[errCap - 1] = 0;
    }
    return ic.ok;
}

size_t ServiceVault::listClients(LightProfile *out, size_t maxN)
{
    char ids[16][32];
    size_t k = store.listIds(ids, 16);
    size_t o = 0;
    for (size_t i = 0; i < k && o < maxN; i++) {
        char buf[4096];
        size_t bn = 0;
        if (!store.readProfile(ids[i], buf, sizeof buf, bn))
            continue;
        Slice cl{buf, bn};
        if (extractLight(cl, out[o]))
            o++;
    }
    return o;
}

bool ServiceVault::select(const char *id)
{
    char ids[16][32];
    size_t k = store.listIds(ids, 16);
    for (size_t i = 0; i < k; i++)
        if (strcmp(ids[i], id) == 0)
            return store.setActive(id);
    return false;
}

bool ServiceVault::activeId(char *out, size_t cap) { return store.getActive(out, cap); }

// packed .seq = N × {uint32 node, uint32 seq} little-endian
static constexpr size_t SEQ_MAX = 256;
static bool findSeqIdx(const uint8_t *buf, size_t bn, uint32_t node, size_t &idx)
{
    for (size_t i = 0; i + 8 <= bn; i += 8) {
        uint32_t nd;
        memcpy(&nd, buf + i, 4);
        if (nd == node) {
            idx = i;
            return true;
        }
    }
    return false;
}

uint32_t ServiceVault::seqFor(const char *id, uint32_t node)
{
    uint8_t buf[SEQ_MAX * 8];
    size_t bn = 0;
    store.readSeq(id, buf, sizeof buf, bn);
    size_t idx;
    if (!findSeqIdx(buf, bn, node, idx))
        return 0;
    uint32_t s;
    memcpy(&s, buf + idx + 4, 4);
    return s;
}

bool ServiceVault::hasSeq(const char *id, uint32_t node)
{
    uint8_t buf[SEQ_MAX * 8];
    size_t bn = 0;
    store.readSeq(id, buf, sizeof buf, bn);
    size_t idx;
    return findSeqIdx(buf, bn, node, idx);
}

void ServiceVault::setSeq(const char *id, uint32_t node, uint32_t seq)
{
    uint8_t buf[SEQ_MAX * 8];
    size_t bn = 0;
    store.readSeq(id, buf, sizeof buf, bn);
    size_t idx;
    if (findSeqIdx(buf, bn, node, idx)) {
        memcpy(buf + idx + 4, &seq, 4);
    } else {
        if (bn + 8 > sizeof buf)
            return;
        memcpy(buf + bn, &node, 4);
        memcpy(buf + bn + 4, &seq, 4);
        bn += 8;
    }
    store.writeSeq(id, buf, bn);
}

size_t ServiceVault::exportEnvelope(char *buf, size_t cap)
{
    IrrigationWeb::JsonWriter w(buf, cap);
    w.beginObject();
    w.keyStr("fmt", "irrig-vault");
    w.keyNum("version", 1);
    w.key("clients");
    w.beginArray();
    char ids[16][32];
    size_t k = store.listIds(ids, 16);
    for (size_t i = 0; i < k; i++) {
        char pb[4096];
        size_t bn = 0;
        if (!store.readProfile(ids[i], pb, sizeof pb - 1, bn))
            continue;
        pb[bn] = 0;
        w.raw(pb); // stored client object verbatim (config carried opaque)
    }
    w.endArray();
    w.endObject();
    return w.done();
}
