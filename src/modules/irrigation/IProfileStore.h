#pragma once
#include "modules/irrigation/IServiceLogReader.h"
#include <cstddef>
#include <cstdint>

// Storage backend for the SERVICO vault (§11.2). LittleFS impl = glue; RAM map = test double.
// Ids are ≤ 31 chars; the vault owns the on-disk layout, this just moves named blobs.
struct IProfileStore {
    virtual ~IProfileStore() = default;
    virtual size_t listIds(char ids[][32], size_t maxIds) = 0; // → count
    virtual bool readProfile(const char *id, char *buf, size_t cap, size_t &outN) = 0;
    virtual bool writeProfile(const char *id, const char *buf, size_t n) = 0; // atomic
    virtual bool removeProfile(const char *id) = 0;
    virtual bool readSeq(const char *id, uint8_t *buf, size_t cap, size_t &outN) = 0;
    virtual bool writeSeq(const char *id, const uint8_t *buf, size_t n) = 0;
    virtual bool getActive(char *out, size_t cap) = 0;
    virtual bool setActive(const char *id) = 0;
    // Log de serviço (§11.2 servico.jsonl). Append de linha JSONL + leitor p/ a aba Log (8c §11.8).
    virtual bool appendLog(const char *line) = 0;
    virtual IrrigationWeb::IServiceLogReader &logReader() = 0;
};
