#include "modules/irrigation/ServiceVault.h"
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
