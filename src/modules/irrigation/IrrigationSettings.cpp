#include "IrrigationSettings.h"
#include "DebugConfiguration.h"
#include "FSCommon.h"
#include <string.h>

static const char *SETTINGS_PATH = "/prefs/irrigation.dat";
static const char *SETTINGS_TMP = "/prefs/irrigation.tmp";

bool migrateIrrigationSettings(const uint8_t *raw, size_t n, IrrigationSettings &out)
{
    uint32_t magic;
    uint16_t version;
    if (n < 6)
        return false;
    memcpy(&magic, raw, 4);
    memcpy(&version, raw + 4, 2);
    if (magic != IrrigationSettings::MAGIC)
        return false;

    if (version == 7) {
        if (n != sizeof(IrrigationSettings))
            return false;
        memcpy(&out, raw, sizeof(out));
        return true;
    }
    if (version == 6) {
        if (n != IRRIGATION_SETTINGS_V6_SIZE)
            return false;
        IrrigationSettings s; // defaults v7 (pinsRemoteLed=-1, masks=0)
        memcpy(&s, raw, IRRIGATION_SETTINGS_V6_SIZE); // v6 é prefixo do v7
        s.version = 7;
        out = s;
        return true;
    }
    if (version == 5) {
        if (n != IRRIGATION_SETTINGS_V5_SIZE)
            return false;
        IrrigationSettings s; // defaults v7 (limiares de bateria + campos v7)
        memcpy(&s, raw, IRRIGATION_SETTINGS_V5_SIZE); // v5 é prefixo do v7
        s.version = 7;
        out = s;
        return true;
    }
    if (version == 4) {
        if (n != IRRIGATION_SETTINGS_V4_SIZE)
            return false;
        IrrigationSettings s; // defaults v7 (localInterlocks zerados = inativos; limiares default; campos v7)
        memcpy(&s, raw, IRRIGATION_SETTINGS_V4_SIZE); // v4 é prefixo do v7
        s.version = 7;
        out = s;
        return true;
    }
    if (version == 3 || version == 2) {
        if (n != IRRIGATION_SETTINGS_V3_SIZE)
            return false;
        IrrigationSettings s; // defaults v7 nos campos novos
        memcpy(&s, raw, IRRIGATION_SETTINGS_V3_SIZE); // layout v2/v3 é prefixo do v7
        s.version = 7;
        if (version == 2) { // bytes de v3 eram padding no v2
            s.pinBtn = -1;
            s.pinLed = -1;
            s.pad2 = 0;
        }
        out = s;
        return true;
    }
    if (version == 1) {
        if (n != IRRIGATION_SETTINGS_V1_SIZE)
            return false;
        IrrigationSettings s; // defaults v7 para os campos novos
        memcpy(&s, raw, IRRIGATION_SETTINGS_V1_SIZE); // layout v1 é prefixo do v2/v3/v7
        s.version = 7;
        out = s;
        return true;
    }
    return false;
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
        LOG_WARN("Irrigation settings invalid (len=%u), using defaults", (unsigned)n);
        return false;
    }
    // Detect upgrade: v1 (40 B), v2/v3 (52 B), v4 (128 B), v5 (176 B), v6 (180 B) ou versão anterior a 7
    uint16_t rawVersion = 0;
    if (n >= 6) {
        memcpy(&rawVersion, raw + 4, 2);
    }
    bool migrated = (n != sizeof(IrrigationSettings)) || (rawVersion < 7);
    s = tmp;
    if (migrated)
        saveIrrigationSettings(s); // regrava já em v7
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
