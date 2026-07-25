#pragma once
#include "modules/irrigation/IProfileStore.h"
#include "modules/irrigation/ServiceBackup.h"
#include <cstddef>

// SERVICO client vault (§11.2). Owns nothing but the store; all persistence via IProfileStore.
class ServiceVault {
  public:
    explicit ServiceVault(IProfileStore &s) : store(s) {}
    // Validate + merge an envelope into the vault. replace=true wipes existing first.
    bool importEnvelope(const char *json, size_t n, bool replace, char *err, size_t errCap);
    // Light metadata for every stored client. → count.
    size_t listClients(IrrigationService::LightProfile *out, size_t maxN);
    bool select(const char *id); // sets active if id exists
    bool activeId(char *out, size_t cap);

  private:
    IProfileStore &store;
};
