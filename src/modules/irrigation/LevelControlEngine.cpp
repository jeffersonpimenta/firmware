#include "modules/irrigation/LevelControlEngine.h"

LevelControlEngine::Rt &LevelControlEngine::rtFor(uint8_t id)
{
    for (auto &s : rt)
        if (s.id == id)
            return s;
    for (auto &s : rt)
        if (s.id == 0) {
            s = Rt{};
            s.id = id;
            return s;
        }
    return rt[0]; // não deve acontecer (rt dimensionado = tabela)
}

size_t LevelControlEngine::evaluate(const LevelControlTable &tbl, const LevelInput *inputs, size_t nInputs,
                                    uint32_t nowMs, LevelIntent *out, size_t maxOut)
{
    size_t no = 0;
    for (size_t i = 0; i < tbl.count() && i < nInputs; i++) {
        const LevelRule *r = tbl.ruleAt(i);
        if (!r)
            break;
        Rt &s = rtFor(r->id);
        const LevelInput &in = inputs[i];

        // --- Boia muda/ausente: fail-safe desliga + alerta 1×/episódio ---
        if (!in.present || !in.fresh) {
            if (s.on || !s.staleAlerted) {
                if (no < maxOut)
                    out[no++] = LevelIntent{r->id, r->targetZoneId, LevelIntent::Act::STALE_STOP, 0};
                s.on = false;
                s.lastOffMs = nowMs;
                s.staleAlerted = true;
            }
            continue;
        }
        s.staleAlerted = false;

        bool wantOn = (in.active == r->ligaQuandoAtivo);

        if (!s.on) {
            if (wantOn && (uint32_t)(nowMs - s.lastOffMs) >= (uint32_t)r->minOffS * 1000u) {
                if (no < maxOut)
                    out[no++] = LevelIntent{r->id, r->targetZoneId, LevelIntent::Act::START, OPEN_CEILING_S};
                s.on = true;
                s.lastOnMs = nowMs;
                s.lastRenewMs = nowMs;
            }
        } else {
            if (!wantOn && (uint32_t)(nowMs - s.lastOnMs) >= (uint32_t)r->minOnS * 1000u) {
                if (no < maxOut)
                    out[no++] = LevelIntent{r->id, r->targetZoneId, LevelIntent::Act::STOP, 0};
                s.on = false;
                s.lastOffMs = nowMs;
            } else if (wantOn && (uint32_t)(nowMs - s.lastRenewMs) >= RENEW_INTERVAL_MS) {
                if (no < maxOut)
                    out[no++] = LevelIntent{r->id, r->targetZoneId, LevelIntent::Act::RENEW, OPEN_CEILING_S};
                s.lastRenewMs = nowMs;
            }
        }
    }
    return no;
}
