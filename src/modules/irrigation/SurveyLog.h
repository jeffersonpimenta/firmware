#pragma once
#include <cstddef>
#include <cstdint>

// §8.5 site survey: uma recepção de beacon registrada no gateway.
struct SurveyPoint {
    uint32_t node = 0;
    uint8_t role = 0;
    uint16_t vbatCentiV = 0;
    uint16_t fwVersion = 0;
    int32_t latE7 = 0, lonE7 = 0;
    bool hasCoord = false;
    int8_t snrQuarterDb = 0;
    int16_t rssiDbm = 0;
    uint32_t uptimeS = 0; // uptime do gateway (s) na captura; sem RTC assumido
};

// Ring em RAM; mantém o mais novo, despeja o mais antigo em CAP. Append (sem dedup):
// o mesmo nó portátil medido em pontos diferentes gera linhas distintas.
class SurveyLog
{
  public:
    static constexpr size_t CAP = 32;
    void add(const SurveyPoint &p)
    {
        ring[head] = p;
        head = (head + 1) % CAP;
        if (n < CAP)
            n++;
    }
    size_t count() const { return n; }
    const SurveyPoint &at(size_t i) const
    {
        size_t start = (n == CAP) ? head : 0; // cheio → head aponta pro mais antigo
        return ring[(start + i) % CAP];
    }
    void clear()
    {
        head = 0;
        n = 0;
    }

  private:
    SurveyPoint ring[CAP];
    size_t head = 0, n = 0;
};
