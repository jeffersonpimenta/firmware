#pragma once
#include "modules/irrigation/IProfileStore.h"

// IProfileStore over FSCom (LittleFS). Layout under a base dir (default "/clientes"):
//   <base>/index        newline-separated client ids
//   <base>/<id>.json    client backup (opaque, streamed)
//   <base>/<id>.seq     packed anti-replay counters
//   <base>/active       active client id
// Writes use staging + rename (atomic, §11.2). Without FSCom every method is a no-op
// returning false/0 — native builds compile it away exactly like LittleFsByteStore.
class LittleFsProfileStore : public IProfileStore {
  public:
    explicit LittleFsProfileStore(const char *base = "/clientes") : base_(base) {}
    void ensureDir(); // best-effort mkdir(base); call once at init

    size_t listIds(char ids[][32], size_t maxIds) override;
    bool readProfile(const char *id, char *buf, size_t cap, size_t &outN) override;
    bool writeProfile(const char *id, const char *buf, size_t n) override;
    bool removeProfile(const char *id) override;
    bool readSeq(const char *id, uint8_t *buf, size_t cap, size_t &outN) override;
    bool writeSeq(const char *id, const uint8_t *buf, size_t n) override;
    bool getActive(char *out, size_t cap) override;
    bool setActive(const char *id) override;

  private:
    const char *base_;
    void indexAdd(const char *id);
    void indexRemove(const char *id);
};
