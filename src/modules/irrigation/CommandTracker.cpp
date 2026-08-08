#include "CommandTracker.h"

bool CommandTracker::track(uint32_t seq, uint32_t node, uint8_t zoneId, uint8_t action, uint16_t durationS,
                           uint8_t attempts, uint32_t nowMs)
{
    // Clamp attempts: 0 could create a phantom FAILED timeout on the first poll
    if (attempts == 0)
        attempts = 1;

    // Procura slot vazio
    for (auto &slot : slots) {
        if (!slot.inUse) {
            slot.inUse = true;
            slot.seq = seq;
            slot.node = node;
            slot.zoneId = zoneId;
            slot.action = action;
            slot.durationS = durationS;
            slot.attemptsLeft = attempts;
            slot.sentAtMs = nowMs;
            return true;
        }
    }
    return false; // fila cheia
}

bool CommandTracker::onAck(uint32_t node, uint32_t ackedSeq)
{
    // Procura slot casado com node e seq
    for (auto &slot : slots) {
        if (slot.inUse && slot.node == node && slot.seq == ackedSeq) {
            slot.inUse = false;
            return true;
        }
    }
    return false; // não encontrado
}

bool CommandTracker::peekZone(uint32_t node, uint32_t seq, uint8_t &zoneIdOut, uint8_t &actionOut) const
{
    for (const auto &slot : slots) {
        if (slot.inUse && slot.node == node && slot.seq == seq) {
            zoneIdOut = slot.zoneId;
            actionOut = slot.action;
            return true;
        }
    }
    return false;
}

bool CommandTracker::retrack(uint32_t newSeq, const Retry &r, uint32_t nowMs)
{
    // Procura slot vazio (RESEND já removeu a pendência anterior)
    for (auto &slot : slots) {
        if (!slot.inUse) {
            slot.inUse = true;
            slot.seq = newSeq;
            slot.node = r.node;
            slot.zoneId = r.zoneId;
            slot.action = r.action;
            slot.durationS = r.durationS;
            slot.attemptsLeft = r.attemptsLeft;
            slot.sentAtMs = nowMs;
            return true;
        }
    }
    return false; // No free slot
}

CommandTracker::Retry CommandTracker::poll(uint32_t nowMs)
{
    Retry result = {Retry::What::NONE};

    // Varre slots em busca de timeout (unsigned subtraction para rollover-safety)
    for (auto &slot : slots) {
        if (!slot.inUse)
            continue;

        if ((nowMs - slot.sentAtMs) >= ACK_TIMEOUT_MS) {
            // Timeout encontrado
            if (slot.attemptsLeft > 1) {
                // Ainda há tentativas: RESEND remove slot e devolve attemptsLeft-1
                result.what = Retry::What::RESEND;
                result.node = slot.node;
                result.zoneId = slot.zoneId;
                result.action = slot.action;
                result.durationS = slot.durationS;
                result.attemptsLeft = slot.attemptsLeft - 1;
                slot.inUse = false; // remove slot
                return result;
            } else {
                // Esgotou tentativas: FAILED remove slot
                result.what = Retry::What::FAILED;
                result.node = slot.node;
                result.zoneId = slot.zoneId;
                result.action = slot.action;
                result.durationS = slot.durationS;
                result.attemptsLeft = slot.attemptsLeft;
                slot.inUse = false; // remove slot
                return result;
            }
        }
    }

    // Sem timeout
    return result;
}
