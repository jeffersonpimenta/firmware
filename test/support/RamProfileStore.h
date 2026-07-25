#pragma once
#include "modules/irrigation/IProfileStore.h"
#include <cstring>
#include <map>
#include <string>

// In-RAM IProfileStore for native tests (no FSCom).
class RamProfileStore : public IProfileStore {
  public:
    size_t listIds(char ids[][32], size_t maxIds) override
    {
        size_t i = 0;
        for (auto &kv : profiles) {
            if (i >= maxIds)
                break;
            strncpy(ids[i], kv.first.c_str(), 31);
            ids[i][31] = 0;
            i++;
        }
        return i;
    }
    bool readProfile(const char *id, char *buf, size_t cap, size_t &outN) override
    {
        auto it = profiles.find(id);
        if (it == profiles.end() || it->second.size() > cap)
            return false;
        memcpy(buf, it->second.data(), it->second.size());
        outN = it->second.size();
        return true;
    }
    bool writeProfile(const char *id, const char *buf, size_t n) override
    {
        profiles[id] = std::string(buf, n);
        return true;
    }
    bool removeProfile(const char *id) override { return profiles.erase(id) > 0; }
    bool readSeq(const char *id, uint8_t *buf, size_t cap, size_t &outN) override
    {
        auto it = seqs.find(id);
        if (it == seqs.end() || it->second.size() > cap)
            return false;
        memcpy(buf, it->second.data(), it->second.size());
        outN = it->second.size();
        return true;
    }
    bool writeSeq(const char *id, const uint8_t *buf, size_t n) override
    {
        seqs[id] = std::string((const char *)buf, n);
        return true;
    }
    bool getActive(char *out, size_t cap) override
    {
        if (active.empty() || active.size() >= cap)
            return false;
        strcpy(out, active.c_str());
        return true;
    }
    bool setActive(const char *id) override
    {
        active = id;
        return true;
    }

  private:
    std::map<std::string, std::string> profiles, seqs;
    std::string active;
};
