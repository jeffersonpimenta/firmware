#pragma once
#include "modules/irrigation/FlashAuditRing.h" // IByteStore, FlashAuditRing::HEADER/REC
#include <stddef.h>
#include <stdint.h>

// IByteStore implementado sobre um arquivo de tamanho fixo no FSCom (LittleFS / Portduino).
// Cada read/write abre, busca o offset e fecha — graváveis são raros (≤1 por evento de auditoria)
// portanto o overhead é aceitável e simplifica o gerenciamento de handles.
//
// Guarda: todos os métodos de I/O são compilados somente quando FSCom está disponível (via
// #ifdef FSCom em FSCommon.h). Sem FSCom os métodos retornam false/0 silenciosamente, o que
// faz FlashAuditRing::begin() reformatar para um anel vazio na RAM — comportamento idêntico ao
// do saveGatewayState nas mesmas condições.
class LittleFsByteStore : public IByteStore
{
  public:
    // path  — caminho absoluto no FSCom (ex.: "/prefs/irrigation_audit.dat")
    // fixedSize — tamanho alocado; determinado pelo chamador (FlashAuditRing::HEADER + N*REC).
    explicit LittleFsByteStore(const char *path, size_t fixedSize);

    // Garante que o arquivo existe e tem ao menos fixedSize bytes (preenche com zeros se necessário).
    // Deve ser chamado uma vez antes de FlashAuditRing::begin().
    // Retorna true se o arquivo já era válido ou foi criado com sucesso.
    bool ensureAllocated();

    // IByteStore
    size_t size() const override { return fixedSize_; }
    bool read(size_t off, void *dst, size_t n) const override;
    bool write(size_t off, const void *src, size_t n) override;

  private:
    const char *path_;
    size_t fixedSize_;
};
