#include "IrrigationSettings.h"
#include "DebugConfiguration.h"
#include "FSCommon.h"
#include <string.h>

static const char *SETTINGS_PATH = "/prefs/irrigation.dat";
static const char *SETTINGS_TMP = "/prefs/irrigation.tmp";

bool migrateIrrigationSettings(const uint8_t *raw, size_t n, IrrigationSettings &out)
{
    // Greenfield v9: aceita apenas blob com magic IRR2 e tamanho exato.
    // Blobs IRR1 (v1..v8) sao rejeitados — caller usa defaults.
    if (n < 6)
        return false;
    uint32_t magic;
    memcpy(&magic, raw, 4);
    if (magic != IrrigationSettings::MAGIC)
        return false;
    if (n != sizeof(IrrigationSettings))
        return false;
    memcpy(&out, raw, sizeof(out));
    return true;
}

bool loadIrrigationSettings(IrrigationSettings &s)
{
#ifdef FSCom
    auto f = FSCom.open(SETTINGS_PATH, FILE_O_READ);
    if (!f)
        return false;
    uint8_t raw[sizeof(IrrigationSettings)];
    size_t n = f.read(raw, sizeof(raw));
    f.close();
    IrrigationSettings tmp;
    if (!migrateIrrigationSettings(raw, n, tmp)) {
        LOG_WARN("Irrigation settings invalid or legacy magic (len=%u), using defaults", (unsigned)n);
        return false;
    }
    s = tmp;
    return true;
#else
    return false;
#endif
}

bool saveIrrigationSettings(const IrrigationSettings &s)
{
#ifdef FSCom
    // Staging + rename: queda de energia no meio não corrompe o arquivo ativo
    auto f = FSCom.open(SETTINGS_TMP, FILE_O_WRITE);
    if (!f)
        return false;
    size_t n = f.write((const uint8_t *)&s, sizeof(s));
    f.close();
    if (n != sizeof(s)) {
        FSCom.remove(SETTINGS_TMP);
        return false;
    }
    FSCom.remove(SETTINGS_PATH);
    if (!renameFile(SETTINGS_TMP, SETTINGS_PATH)) {
        LOG_ERROR("Irrigation settings rename failed");
        FSCom.remove(SETTINGS_TMP);
        return false;
    }
    return true;
#else
    return false;
#endif
}
