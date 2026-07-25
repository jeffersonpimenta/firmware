#include "modules/irrigation/ServiceBackup.h"
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

} // namespace IrrigationService
