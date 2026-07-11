#pragma once
#include <stddef.h>
#include <stdint.h>

// Remonta o blob de SET_CONFIG (spec §5.4/§5.5): uma transferência em curso por vez,
// fragmentos fora de ordem e duplicados tolerados, CRC32 validado no fechamento.
class FragmentReassembler
{
  public:
    static constexpr size_t MAX_CONFIG_LEN = 512;
    static constexpr uint8_t MAX_FRAGS = 16;
    static constexpr uint32_t TRANSFER_TIMEOUT_MS = 30000;

    enum class Add : uint8_t { ACCEPTED, COMPLETE, TOO_BIG, BAD_CRC, INVALID };

    Add add(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen, uint8_t fragIndex, uint8_t fragCount,
            const uint8_t *data, uint8_t fragLen, uint32_t nowMs);
    const uint8_t *blob() const { return buf; }
    uint16_t blobLen() const { return curTotalLen; }
    uint32_t epoch() const { return curEpoch; }
    void reset();

  private:
    bool sameTransfer(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen, uint8_t fragCount) const;
    bool active = false;
    uint32_t curSender = 0;
    uint32_t curEpoch = 0;
    uint32_t curCrc = 0;
    uint16_t curTotalLen = 0;
    uint8_t curFragCount = 0;
    uint16_t receivedMask = 0;
    uint32_t lastMs = 0;
    uint8_t buf[MAX_CONFIG_LEN];
};
