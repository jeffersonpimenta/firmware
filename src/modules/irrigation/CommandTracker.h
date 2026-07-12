#pragma once
#include <stddef.h>
#include <stdint.h>

// Rastreador de comandos aguardando ACK com retry automático (spec §5.1).
// Array fixo de MAX_PENDING slots; timeout de 8 s; unsigned-subtraction para rollover-safety.
class CommandTracker
{
  public:
    static constexpr size_t MAX_PENDING = 8;
    static constexpr uint32_t ACK_TIMEOUT_MS = 8000;

    struct Retry {
        enum class What : uint8_t { NONE, RESEND, FAILED } what = What::NONE;
        uint32_t node = 0;
        uint8_t zoneId = 0;
        uint8_t action = 0;      // 0 fechar, 1 abrir
        uint16_t durationS = 0;
        uint8_t attemptsLeft = 0;
    };

    // Registra comando em espera. Retorna false se fila está cheia.
    bool track(uint32_t seq, uint32_t node, uint8_t zoneId, uint8_t action, uint16_t durationS, uint8_t attempts,
               uint32_t nowMs);

    // Confirma recebimento do ACK. Retorna true se encontrou e removeu a pendência.
    bool onAck(uint32_t node, uint32_t ackedSeq);

    // Registra reenvio após timeout (chamado após glue reenvia com novo seq).
    void retrack(uint32_t newSeq, const Retry &r, uint32_t nowMs);

    // Verifica timeout (nowMs - sentAt >= 8000): retorna 1 por chamada.
    // RESEND: removeu slot e devolve attemptsLeft-1 (glue chama retrack com novo seq).
    // FAILED: removeu slot e esgotou attempts (glue alerta).
    // NONE: sem timeout.
    Retry poll(uint32_t nowMs);

  private:
    struct Slot {
        bool inUse = false;
        uint32_t seq = 0;
        uint32_t node = 0;
        uint32_t sentAtMs = 0;
        uint8_t zoneId = 0;
        uint8_t action = 0;
        uint8_t attemptsLeft = 0;
        uint16_t durationS = 0;
    };
    Slot slots[MAX_PENDING];
};
