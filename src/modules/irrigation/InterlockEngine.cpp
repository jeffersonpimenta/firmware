#include "modules/irrigation/InterlockEngine.h"
#include "modules/irrigation/InterlockTable.h"
static_assert(InterlockTable::MAX == INTERLOCK_MAX, "engine array size deve casar com InterlockTable::MAX");

bool evalCondition(uint8_t condicao, bool active, int32_t valueCenti,
                   int32_t thresholdCenti, uint16_t histCenti, bool &latched)
{
    switch (condicao) {
    case COND_ATIVO:   latched = active;  break;
    case COND_INATIVO: latched = !active; break;
    case COND_MENOR_QUE:
        if (valueCenti < thresholdCenti) latched = true;
        else if (valueCenti >= thresholdCenti + (int32_t)histCenti) latched = false;
        break; // dentro da banda: mantém latched
    case COND_MAIOR_QUE:
        if (valueCenti > thresholdCenti) latched = true;
        else if (valueCenti <= thresholdCenti - (int32_t)histCenti) latched = false;
        break;
    default: latched = false; break;
    }
    return latched;
}

const SensorSnapshot *InterlockEngine::findSnap(const SensorSnapshot *s, size_t n, uint32_t node, uint8_t idx) {
    for (size_t i = 0; i < n; i++) if (s[i].node == node && s[i].sensorIdx == idx) return &s[i];
    return nullptr;
}

bool InterlockEngine::ruleCoversZone(const InterlockRule &r, uint8_t zoneId) {
    if (r.todas) return true;
    for (uint8_t z : r.zoneIds) { if (z == 0) break; if (z == zoneId) return true; }
    return false;
}

uint8_t InterlockEngine::evaluate(const InterlockTable &tbl, const SensorSnapshot *snaps, size_t nSnaps) {
    lastTbl = &tbl;
    uint8_t cap = 0; // 0 = sem limite
    for (size_t i = 0; i < INTERLOCK_MAX; i++) {
        const InterlockRule *r = tbl.ruleAtSlot(i);
        fired[i] = false;
        if (!r) { latched[i] = false; continue; }
        if (r->tipo == IL_SIMULTANEIDADE) {
            if (r->maxAbertas > 0 && (cap == 0 || r->maxAbertas < cap)) cap = r->maxAbertas;
            latched[i] = false; // Fix 2: evita latch obsoleto se tipo de slot mudar
            continue;
        }
        const SensorSnapshot *sn = findSnap(snaps, nSnaps, r->node, r->sensorIdx);
        if (!sn || !sn->present) { latched[i] = false; continue; } // sem dado = não dispara
        fired[i] = evalCondition(r->condicao, sn->active, sn->valueCenti,
                                 r->valorCenti, r->histereseCenti, latched[i]);
    }
    return cap;
}

ZoneVerdict InterlockEngine::zoneVerdict(uint8_t zoneId) const {
    ZoneVerdict v;
    if (!lastTbl) return v;
    for (size_t i = 0; i < INTERLOCK_MAX; i++) {
        if (!fired[i]) continue;
        const InterlockRule *r = lastTbl->ruleAtSlot(i);
        if (!r || r->tipo != IL_SENSOR) continue;
        if (!ruleCoversZone(*r, zoneId)) continue;
        v.bloqueada = true; v.ruleId = r->id;
        if (r->acao == ACAO_FECHAR_E_BLOQUEAR) v.deveFechar = true;
    }
    return v;
}

LocalReplicaOut evalLocalInterlocks(const IrrigationSettings::LocalInterlock *rules, size_t nRules,
                                    const IrrigationProto::SensorReading *readings, size_t nReadings,
                                    bool *latched)
{
    LocalReplicaOut out;
    for (size_t i = 0; i < nRules; i++) {
        const auto &r = rules[i];
        if (r.saidasValvMask == 0 && r.saidasGpoMask == 0) { if (latched) latched[i] = false; continue; } // inativo
        const IrrigationProto::SensorReading *rd = nullptr;
        for (size_t k = 0; k < nReadings; k++) if (readings[k].id == r.sensorIdx) { rd = &readings[k]; break; }
        if (!rd) { if (latched) latched[i] = false; continue; }
        bool active = rd->valueCenti != 0; // digital: 0/100
        bool fire = evalCondition(r.condicao, active, rd->valueCenti,
                                  r.valorCenti, r.histereseCenti, latched[i]);
        if (fire) {
            out.bloquearValvMask |= r.saidasValvMask;
            out.bloquearGpoMask  |= r.saidasGpoMask;
            if (r.acao == ACAO_FECHAR_E_BLOQUEAR) {
                out.fecharValvMask |= r.saidasValvMask;
                out.fecharGpoMask  |= r.saidasGpoMask;
            }
        }
    }
    return out;
}
