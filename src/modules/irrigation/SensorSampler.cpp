#include "SensorSampler.h"

void SensorSampler::configure(const IrrigationSettings &s)
{
    cfg = s;
    for (auto &sl : st)
        sl = SlotState{};
    pending = false;
    lastReportMs = 0;
}

int16_t SensorSampler::calibrate(const IrrigationSettings::SensorSlot &sl, uint16_t adc) const
{
    if (sl.adcMax <= sl.adcMin)
        return sl.engMin; // calibração degenerada: valor fixo, sem div/0
    if (adc <= sl.adcMin)
        return sl.engMin;
    if (adc >= sl.adcMax)
        return sl.engMax;
    int32_t num = (int32_t)(adc - sl.adcMin) * (sl.engMax - sl.engMin);
    return (int16_t)(sl.engMin + num / (sl.adcMax - sl.adcMin));
}

void SensorSampler::tick(uint32_t nowMs, ISensorReader &rd)
{
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        const auto &sl = cfg.sensores[i];
        auto &s = st[i];
        if (sl.pino < 0)
            continue;
        if (sl.tipo == 1) { // analógico
            uint32_t periodMs = (sl.amostragemS ? sl.amostragemS : DEFAULT_AMOSTRAGEM_S) * 1000u;
            if (s.valid && nowMs - s.lastSampleMs < periodMs)
                continue;
            s.valueCenti = calibrate(sl, rd.readAdc(sl.pino));
            s.lastSampleMs = nowMs;
            s.valid = true;
            // Histerese: banda = 2% do span (mínimo 1 centi)
            int32_t span = (int32_t)sl.engMax - sl.engMin;
            if (span < 0)
                span = -span;
            int32_t band = span * HYST_PCT / 100;
            if (band < 1)
                band = 1;
            int32_t delta = (int32_t)s.valueCenti - s.reportedCenti;
            if (delta < 0)
                delta = -delta;
            if (s.everReported && delta > band)
                pending = true;
        } else { // digital
            bool raw = rd.readLevel(sl.pino);
            bool active = (sl.flags & 1) ? !raw : raw;
            uint16_t db = sl.debounceMs ? sl.debounceMs : DEFAULT_DEBOUNCE_MS;
            if (!s.everSampled || active != s.lastRaw) {
                s.lastRaw = active;
                s.rawSinceMs = nowMs;
                s.everSampled = true;
                continue; // aguarda estabilidade
            }
            if (nowMs - s.rawSinceMs < db)
                continue;
            int16_t v = active ? 100 : 0;
            if (s.valid && v != s.valueCenti && s.everReported)
                pending = true;
            s.valueCenti = v;
            s.valid = true;
        }
    }
}

size_t SensorSampler::readings(IrrigationProto::SensorReading out[IrrigationSettings::MAX_SENSORS]) const
{
    size_t n = 0;
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        if (cfg.sensores[i].pino < 0 || !st[i].valid)
            continue;
        out[n++] = {i, cfg.sensores[i].tipo, st[i].valueCenti};
    }
    return n;
}

bool SensorSampler::earlyHeartbeatDue(uint32_t nowMs) const
{
    return pending && (nowMs - lastReportMs >= EARLY_HB_MIN_INTERVAL_MS);
}

void SensorSampler::noteReported(uint32_t nowMs)
{
    pending = false;
    lastReportMs = nowMs;
    for (uint8_t i = 0; i < IrrigationSettings::MAX_SENSORS; i++) {
        if (!st[i].valid)
            continue;
        st[i].reportedCenti = st[i].valueCenti;
        st[i].everReported = true;
    }
}
