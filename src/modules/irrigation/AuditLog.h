#pragma once
#include <stddef.h>
#include <stdint.h>

// §8.9 — origens/ações/resultados do log de auditoria.
enum class AuditOrigin : uint8_t {
    SISTEMA = 0, CRONOGRAMA, PAINEL, PORTAL_CAMPO, BOTAO_FISICO,
    ENTRADA_FISICA, INTERTRAVAMENTO, FAILSAFE_TIMER, SERVICO
};
enum class AuditAction : uint8_t {
    ABRIR = 0, FECHAR, PULSO, GPO_ON, GPO_OFF, PAREAR, FACTORY_RESET,
    CONFIG_EPOCH, SAFE_MODE_IN, SAFE_MODE_OUT, TAMPER, REBOOT,
    HIBERNA_IN, HIBERNA_OUT
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
    static constexpr uint32_t MAGIC = 0x49414C31; // "IAL1"

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
