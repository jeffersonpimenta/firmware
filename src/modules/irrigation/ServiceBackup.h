#pragma once
#include <cstddef>
#include <cstdint>

namespace IrrigationService {

size_t base64Encode(const uint8_t *in, size_t n, char *out, size_t cap);
int base64Decode(const char *in, size_t n, uint8_t *out, size_t cap);

// ── Structural JSON scanner (brace/bracket/string-aware; slices point into source) ──
struct Slice {
    const char *p = nullptr;
    size_t n = 0;
};
// Value slice for a top-level key in an object. false = absent.
bool jsonMember(const char *obj, size_t n, const char *key, Slice &out);
// Iterate array elements; cb returns false to stop early.
bool jsonForEachArray(Slice arr, void *ctx, bool (*cb)(void *, Slice elem));
// Iterate object members (dynamic keys); cb(ctx, keyWithoutQuotes, valueSlice).
bool jsonForEachMember(Slice obj, void *ctx, bool (*cb)(void *, Slice key, Slice val));
// Scalar getters on an object slice.
bool jsonStr(Slice obj, const char *key, char *out, size_t cap);
bool jsonInt(Slice obj, const char *key, int64_t &out);

// ── Multi-client envelope ───────────────────────────────────────────────────
// Iterate the clients[] array of an envelope; cb returns false to stop early.
bool envelopeForEachClient(const char *json, size_t n, void *ctx, bool (*cb)(void *, Slice client));
// fmt=="irrig-vault", version==1, each client has id + gateway + canal.psk_b64 (valid base64).
bool validateEnvelope(const char *json, size_t n, char *err, size_t errCap);

} // namespace IrrigationService
