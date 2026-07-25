#include "modules/irrigation/ServiceBackup.h"
#include <cstdlib>
#include <cstring>

namespace IrrigationService {

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64Encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t need = ((n + 2) / 3) * 4;
    if (need > cap)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16;
        if (i + 1 < n)
            v |= in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? kB64[v & 63] : '=';
    }
    return o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

int base64Decode(const char *in, size_t n, uint8_t *out, size_t cap)
{
    if (n % 4 != 0)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int a = b64val(in[i]), b = b64val(in[i + 1]);
        if (a < 0 || b < 0)
            return -1;
        bool p2 = in[i + 2] == '=', p3 = in[i + 3] == '=';
        int c = p2 ? 0 : b64val(in[i + 2]);
        int d = p3 ? 0 : b64val(in[i + 3]);
        if ((!p2 && c < 0) || (!p3 && d < 0))
            return -1;
        uint32_t v = (a << 18) | (b << 12) | (c << 6) | d;
        if (o >= cap)
            return -1;
        out[o++] = (v >> 16) & 0xff;
        if (!p2) {
            if (o >= cap)
                return -1;
            out[o++] = (v >> 8) & 0xff;
        }
        if (!p3) {
            if (o >= cap)
                return -1;
            out[o++] = v & 0xff;
        }
    }
    return (int)o;
}

// ── Structural JSON scanner ─────────────────────────────────────────────────

static const char *skipWs(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        ++p;
    return p;
}

// p at value start → pointer just past the value (string/object/array/scalar).
static const char *skipValue(const char *p, const char *end)
{
    p = skipWs(p, end);
    if (p >= end)
        return end;
    if (*p == '"') {
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end)
                ++p;
            ++p;
        }
        return (p < end) ? p + 1 : end;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int d = 0;
        bool ins = false;
        for (; p < end; ++p) {
            char c = *p;
            if (ins) {
                if (c == '\\') {
                    ++p;
                    continue;
                }
                if (c == '"')
                    ins = false;
                continue;
            }
            if (c == '"')
                ins = true;
            else if (c == open)
                ++d;
            else if (c == close && --d == 0)
                return p + 1;
        }
        return end;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        ++p;
    return p;
}

// Iterate object members; cb(ctx, keySlice(without quotes), valueSlice). Stop if cb false.
static bool forEachObjMember(const char *obj, size_t n, void *ctx, bool (*cb)(void *, Slice, Slice))
{
    const char *p = obj, *end = obj + n;
    p = skipWs(p, end);
    if (p >= end || *p != '{')
        return false;
    ++p;
    while (p < end) {
        p = skipWs(p, end);
        if (p < end && *p == '}')
            return true;
        if (p >= end || *p != '"')
            return false;
        const char *ks = p + 1, *ke = ks;
        while (ke < end && *ke != '"') {
            if (*ke == '\\')
                ++ke;
            ++ke;
        }
        p = (ke < end) ? ke + 1 : end;
        p = skipWs(p, end);
        if (p >= end || *p != ':')
            return false;
        ++p;
        p = skipWs(p, end);
        const char *vs = p, *ve = skipValue(p, end);
        Slice ks2{ks, (size_t)(ke - ks)}, vs2{vs, (size_t)(ve - vs)};
        if (!cb(ctx, ks2, vs2))
            return true;
        p = skipWs(ve, end);
        if (p < end && *p == ',')
            ++p;
    }
    return true;
}

struct MemberFind {
    const char *key;
    size_t klen;
    Slice out;
    bool found;
};
static bool memberFindCb(void *c, Slice k, Slice v)
{
    auto *m = (MemberFind *)c;
    if (k.n == m->klen && memcmp(k.p, m->key, m->klen) == 0) {
        m->out = v;
        m->found = true;
        return false; // stop
    }
    return true;
}

bool jsonMember(const char *obj, size_t n, const char *key, Slice &out)
{
    MemberFind m{key, strlen(key), {}, false};
    forEachObjMember(obj, n, &m, memberFindCb);
    if (m.found)
        out = m.out;
    return m.found;
}

bool jsonForEachMember(Slice obj, void *ctx, bool (*cb)(void *, Slice, Slice))
{
    return forEachObjMember(obj.p, obj.n, ctx, cb);
}

bool jsonForEachArray(Slice arr, void *ctx, bool (*cb)(void *, Slice))
{
    const char *p = arr.p, *end = arr.p + arr.n;
    p = skipWs(p, end);
    if (p >= end || *p != '[')
        return false;
    ++p;
    while (p < end) {
        p = skipWs(p, end);
        if (p < end && *p == ']')
            return true;
        const char *vs = p, *ve = skipValue(p, end);
        Slice el{vs, (size_t)(ve - vs)};
        if (!cb(ctx, el))
            return true;
        p = skipWs(ve, end);
        if (p < end && *p == ',')
            ++p;
    }
    return true;
}

bool jsonInt(Slice obj, const char *key, int64_t &out)
{
    Slice v;
    if (!jsonMember(obj.p, obj.n, key, v) || v.n == 0)
        return false;
    out = strtoll(v.p, nullptr, 10);
    return true;
}

bool jsonStr(Slice obj, const char *key, char *out, size_t cap)
{
    Slice v;
    if (!jsonMember(obj.p, obj.n, key, v) || v.n < 2 || v.p[0] != '"')
        return false;
    size_t o = 0;
    for (size_t i = 1; i + 1 < v.n && o + 1 < cap; ++i) {
        char c = v.p[i];
        if (c == '\\' && i + 2 < v.n)
            c = v.p[++i];
        out[o++] = c;
    }
    out[o] = 0;
    return true;
}

} // namespace IrrigationService
