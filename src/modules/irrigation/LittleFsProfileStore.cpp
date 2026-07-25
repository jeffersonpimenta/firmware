#include "modules/irrigation/LittleFsProfileStore.h"
#include "FSCommon.h"
#include <stdio.h>
#include <string.h>

#ifdef FSCom

// Staged write: tmp → rename. Returns true on success.
static bool fsWrite(const char *path, const void *buf, size_t n)
{
    char tmp[80];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    auto f = FSCom.open(tmp, FILE_O_WRITE);
    if (!f)
        return false;
    size_t w = f.write((const uint8_t *)buf, n);
    f.close();
    if (w != n) {
        FSCom.remove(tmp);
        return false;
    }
    FSCom.remove(path);
    if (!renameFile(tmp, path)) {
        FSCom.remove(tmp);
        return false;
    }
    return true;
}

static bool fsRead(const char *path, void *buf, size_t cap, size_t &outN)
{
    auto f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return false;
    outN = f.read((uint8_t *)buf, cap);
    f.close();
    return true;
}

static void joinPath(char *dst, size_t cap, const char *base, const char *id, const char *ext)
{
    snprintf(dst, cap, "%s/%s%s", base, id, ext);
}

#endif // FSCom

void LittleFsProfileStore::ensureDir()
{
#ifdef FSCom
    FSCom.mkdir(base_);
#endif
}

size_t LittleFsProfileStore::listIds(char ids[][32], size_t maxIds)
{
#ifdef FSCom
    char idx[64];
    joinPath(idx, sizeof idx, base_, "index", "");
    char buf[1024];
    size_t n = 0;
    if (!fsRead(idx, buf, sizeof buf - 1, n))
        return 0;
    buf[n] = 0;
    size_t count = 0;
    char *line = strtok(buf, "\n");
    while (line && count < maxIds) {
        if (line[0]) {
            strncpy(ids[count], line, 31);
            ids[count][31] = 0;
            count++;
        }
        line = strtok(nullptr, "\n");
    }
    return count;
#else
    (void)ids;
    (void)maxIds;
    return 0;
#endif
}

bool LittleFsProfileStore::readProfile(const char *id, char *buf, size_t cap, size_t &outN)
{
#ifdef FSCom
    char path[80];
    joinPath(path, sizeof path, base_, id, ".json");
    return fsRead(path, buf, cap, outN);
#else
    (void)id;
    (void)buf;
    (void)cap;
    (void)outN;
    return false;
#endif
}

bool LittleFsProfileStore::writeProfile(const char *id, const char *buf, size_t n)
{
#ifdef FSCom
    char path[80];
    joinPath(path, sizeof path, base_, id, ".json");
    if (!fsWrite(path, buf, n))
        return false;
    indexAdd(id);
    return true;
#else
    (void)id;
    (void)buf;
    (void)n;
    return false;
#endif
}

bool LittleFsProfileStore::removeProfile(const char *id)
{
#ifdef FSCom
    char path[80];
    joinPath(path, sizeof path, base_, id, ".json");
    FSCom.remove(path);
    joinPath(path, sizeof path, base_, id, ".seq");
    FSCom.remove(path);
    indexRemove(id);
    return true;
#else
    (void)id;
    return false;
#endif
}

bool LittleFsProfileStore::readSeq(const char *id, uint8_t *buf, size_t cap, size_t &outN)
{
#ifdef FSCom
    char path[80];
    joinPath(path, sizeof path, base_, id, ".seq");
    return fsRead(path, buf, cap, outN);
#else
    (void)id;
    (void)buf;
    (void)cap;
    (void)outN;
    return false;
#endif
}

bool LittleFsProfileStore::writeSeq(const char *id, const uint8_t *buf, size_t n)
{
#ifdef FSCom
    char path[80];
    joinPath(path, sizeof path, base_, id, ".seq");
    return fsWrite(path, buf, n);
#else
    (void)id;
    (void)buf;
    (void)n;
    return false;
#endif
}

bool LittleFsProfileStore::getActive(char *out, size_t cap)
{
#ifdef FSCom
    char path[80];
    joinPath(path, sizeof path, base_, "active", "");
    size_t n = 0;
    if (!fsRead(path, out, cap - 1, n) || n == 0)
        return false;
    out[n] = 0;
    return true;
#else
    (void)out;
    (void)cap;
    return false;
#endif
}

bool LittleFsProfileStore::setActive(const char *id)
{
#ifdef FSCom
    char path[80];
    joinPath(path, sizeof path, base_, "active", "");
    return fsWrite(path, id, strlen(id));
#else
    (void)id;
    return false;
#endif
}

bool LittleFsProfileStore::appendLog(const char *line)
{
#ifdef FSCom
    char path[80];
    snprintf(path, sizeof path, "%s/servico.jsonl", base_);
    auto f = FSCom.open(path, "a"); // append (rotação = follow-on; o reader só lê o tail)
    if (!f)
        return false;
    f.write((const uint8_t *)line, strlen(line));
    f.write((const uint8_t *)"\n", 1);
    f.close();
    return true;
#else
    (void)line;
    return false;
#endif
}

void LittleFsProfileStore::LogReader::forEachLine(size_t maxLines, void *ctx, void (*cb)(void *, const char *))
{
#ifdef FSCom
    char path[80];
    snprintf(path, sizeof path, "%s/servico.jsonl", s->base_);
    auto f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return;
    const size_t CAP = 8192;
    size_t sz = f.size();
    size_t off = (sz > CAP) ? sz - CAP : 0; // lê só o tail
    if (off)
        f.seek(off);
    static char buf[CAP + 1];
    size_t n = f.read((uint8_t *)buf, CAP);
    f.close();
    buf[n] = 0;
    char *p = buf;
    if (off) { // descarta 1ª linha parcial
        char *nl = strchr(p, '\n');
        if (nl)
            p = nl + 1;
    }
    char *lines[128];
    size_t cnt = 0;
    while (*p && cnt < 128) {
        char *nl = strchr(p, '\n');
        if (!nl) {
            lines[cnt++] = p;
            break;
        }
        *nl = 0;
        if (*p)
            lines[cnt++] = p;
        p = nl + 1;
    }
    size_t start = (cnt > maxLines) ? cnt - maxLines : 0;
    for (size_t i = start; i < cnt; i++)
        cb(ctx, lines[i]);
#else
    (void)maxLines;
    (void)ctx;
    (void)cb;
#endif
}

void LittleFsProfileStore::indexAdd(const char *id)
{
#ifdef FSCom
    char ids[16][32];
    size_t k = listIds(ids, 16);
    for (size_t i = 0; i < k; i++)
        if (strcmp(ids[i], id) == 0)
            return; // already present
    char idx[64];
    joinPath(idx, sizeof idx, base_, "index", "");
    char buf[1024];
    size_t n = 0;
    fsRead(idx, buf, sizeof buf - 64, n);
    int m = snprintf(buf + n, sizeof buf - n, "%s\n", id);
    if (m > 0)
        fsWrite(idx, buf, n + (size_t)m);
#else
    (void)id;
#endif
}

void LittleFsProfileStore::indexRemove(const char *id)
{
#ifdef FSCom
    char ids[16][32];
    size_t k = listIds(ids, 16);
    char idx[64];
    joinPath(idx, sizeof idx, base_, "index", "");
    char buf[1024];
    size_t o = 0;
    for (size_t i = 0; i < k; i++) {
        if (strcmp(ids[i], id) == 0)
            continue;
        int m = snprintf(buf + o, sizeof buf - o, "%s\n", ids[i]);
        if (m > 0)
            o += (size_t)m;
    }
    fsWrite(idx, buf, o);
#else
    (void)id;
#endif
}
