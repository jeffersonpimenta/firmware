#include "IrrigationSettings.h"
#include "DebugConfiguration.h"
#include "FSCommon.h"

static const char *SETTINGS_PATH = "/prefs/irrigation.dat";
static const char *SETTINGS_TMP = "/prefs/irrigation.tmp";

bool loadIrrigationSettings(IrrigationSettings &s)
{
#ifdef FSCom
    auto f = FSCom.open(SETTINGS_PATH, FILE_O_READ);
    if (!f)
        return false;
    IrrigationSettings tmp;
    size_t n = f.read((uint8_t *)&tmp, sizeof(tmp));
    f.close();
    if (n != sizeof(tmp) || tmp.magic != IrrigationSettings::MAGIC || tmp.version != 1) {
        LOG_WARN("Irrigation settings invalid, using defaults");
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
