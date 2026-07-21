#pragma once
#include <stddef.h>
#include <stdint.h>

// §8.9 — origens/ações/resultados do log de auditoria.
enum class AuditOrigin : uint8_t {
    SISTEMA = 0, CRONOGRAMA, PAINEL, PORTAL_CAMPO, BOTAO_FISICO,
    ENTRADA_FISICA, INTERTRAVAMENTO, FAILSAFE_TIMER, SERVICO
};
// Valores são ABI persistida (log em flash) e contrato com os rótulos do JS (portal/app.js):
// só APPEND no fim, nunca reordenar/remover. FACTORY_RESET (log é apagado no reset, não se
// audita) e HIBERNA_IN/OUT são reservados p/ fase futura — ainda não emitidos por auditEvent.
enum class AuditAction : uint8_t {
    ABRIR = 0, FECHAR, PULSO, GPO_ON, GPO_OFF, PAREAR, FACTORY_RESET,
    CONFIG_EPOCH, SAFE_MODE_IN, SAFE_MODE_OUT, TAMPER, REBOOT,
    HIBERNA_IN, HIBERNA_OUT,
    CMD_REJEITADO, // comando NACKado antes do despacho (motivo em target = NackReason)
};
enum class AuditResult : uint8_t { OK = 0, NACK, TIMEOUT };

struct AuditRecord {
    uint32_t tsSecs = 0; // epoch local; 0 = sem RTC no momento
    uint8_t origin = 0;  // AuditOrigin
    uint8_t action = 0;  // AuditAction
    uint8_t target = 0;  // válvula/GPO/zona/código conforme a ação
    uint8_t result = 0;  // AuditResult
    uint32_t node = 0;   // ator remoto (0 = local)
    uint32_t seq = 0;    // seq da mensagem de rádio (0 = ação local)
};

// Ring somente-append. Storage é do chamador (estação: 100; gateway: maior).
class AuditLog
{
  public:
    // "IAL1" — formato on-disk: magic(4) + count(2 LE) + reservado(2, escritos 0, ignorados) + N×16 bytes + CRC32(4).
    // O campo count é uint16_t, portanto a serialização limita-se a 65535 registros
    // (rings maiores emitem apenas os 65535 mais recentes).
    static constexpr uint32_t MAGIC = 0x49414C31;

    AuditLog(AuditRecord *storage, size_t cap) : buf(storage), cap(cap) {}

    void append(const AuditRecord &r);
    size_t size() const { return num; }
    // i = 0 é o MAIS RECENTE.
    const AuditRecord &at(size_t i) const;
    size_t serialize(uint8_t *out, size_t outCap) const; // 0 = não coube
    bool deserialize(const uint8_t *in, size_t n);       // mantém os mais recentes se cap menor
    void clear();

  private:
    AuditRecord *buf;
    size_t cap;
    size_t writeIdx = 0;
    size_t num = 0;
};
