#pragma once
#include "modules/irrigation/AuditLog.h" // AuditRecord
#include <stddef.h>
#include <stdint.h>

// Abstrai o armazenamento de bytes (arquivo LittleFS no gateway; vetor nos testes).
class IByteStore {
  public:
    virtual ~IByteStore() = default;
    virtual size_t size() const = 0;
    virtual bool read(size_t off, void *dst, size_t n) const = 0;
    virtual bool write(size_t off, const void *src, size_t n) = 0;
};

// Layout: header(16B) + N×16B slots. header = magic(4) head(4) count(4) crc(4).
// `head` = índice do próximo slot a escrever; `count` satura em capacidade.
class FlashAuditRing {
  public:
    static constexpr uint32_t MAGIC = 0x49415232; // "IAR2"
    static constexpr size_t HEADER = 16;
    static constexpr size_t REC = sizeof(AuditRecord); // 16

    explicit FlashAuditRing(IByteStore &store) : store(store) {}
    bool begin();                 // lê/valida header; se inválido, formata (zera)
    void append(const AuditRecord &r);
    size_t count() const { return num; }
    size_t capacity() const { return (store.size() >= HEADER + REC) ? (store.size() - HEADER) / REC : 0; }
    bool at(size_t i, AuditRecord &out) const; // i=0 = mais recente
    void clear();

    // Escreve até `cap` bytes. Retorna bytes escritos (trunca no último registro
    // que couber inteiro). `maxRecords` limita a quantos registros no máximo.
    size_t toCsv(char *buf, size_t cap, size_t maxRecords) const;
    size_t toJson(char *buf, size_t cap, size_t maxRecords) const;

  private:
    IByteStore &store;
    uint32_t head = 0;
    uint32_t num = 0;
    void writeHeader();
    size_t slotOffset(uint32_t idx) const { return HEADER + (idx % capacity()) * REC; }
};
