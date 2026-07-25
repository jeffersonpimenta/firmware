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

// ── Light profile (operational fields kept in RAM; §11.2 parse-light) ────────
struct LightStation {
    uint32_t node = 0;
    char name[24] = {0};
    int32_t lat = 0, lon = 0;
};
struct LightProfile {
    char id[32] = {0};
    char nome[32] = {0};
    char canalNome[13] = {0}; // Meshtastic channel name ≤ 12 + NUL
    char pskB64[48] = {0};
    uint8_t preset = 0; // ModemPreset enum value
    uint32_t gateway = 0;
    LightStation estacoes[16];
    uint8_t estacaoCount = 0;
};
uint8_t presetFromString(const char *s); // "LONG_FAST"→0…; default LONG_FAST(0)
const char *presetToString(uint8_t p);
uint32_t parseNodeHex(const char *s); // "!a1b2c3d4"→0xa1b2c3d4; leading '!' optional
bool extractLight(Slice client, LightProfile &out);

// ── §5.5 client backup assembly (glue builds sub-arrays via IrrigationWeb::build*) ──
struct BackupSource {
    const char *id = "";
    const char *nome = "";
    const char *canalNome = "";
    const char *pskB64 = "";
    uint8_t preset = 0;
    uint32_t gateway = 0;
    const char *estacoesJson = "[]";      // "[{no,nome,lat,lon}]"
    const char *snapshotEpochJson = "{}"; // "{\"!e5f6a7b8\":17}"
    const char *zonasJson = "[]";
    const char *programasJson = "[]";
    const char *intertravamentosJson = "[]";
    const char *gruposJson = "[]";
    const char *sensorNamesJson = "[]";
    const char *seqJson = ""; // "" to omit
};
size_t buildClientBackup(const BackupSource &s, char *buf, size_t cap);

} // namespace IrrigationService
