#include "FragmentReassembler.h"
#include "IrrigationProtocol.h"
#include <string.h>

void FragmentReassembler::reset()
{
    active = false;
    receivedMask = 0;
}

bool FragmentReassembler::sameTransfer(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen,
                                       uint8_t fragCount) const
{
    return curSender == sender && curEpoch == epoch && curCrc == crc && curTotalLen == totalLen &&
           curFragCount == fragCount;
}

FragmentReassembler::Add FragmentReassembler::add(uint32_t sender, uint32_t epoch, uint32_t crc, uint16_t totalLen,
                                                  uint8_t fragIndex, uint8_t fragCount, const uint8_t *data,
                                                  uint8_t fragLen, uint32_t nowMs)
{
    if (totalLen > MAX_CONFIG_LEN || fragCount > MAX_FRAGS)
        return Add::TOO_BIG;
    if (fragCount == 0 || fragIndex >= fragCount || fragLen == 0)
        return Add::INVALID;

    bool stale = active && (nowMs - lastMs) > TRANSFER_TIMEOUT_MS;
    if (!active || stale || !sameTransfer(sender, epoch, crc, totalLen, fragCount)) {
        active = true;
        curSender = sender;
        curEpoch = epoch;
        curCrc = crc;
        curTotalLen = totalLen;
        curFragCount = fragCount;
        receivedMask = 0;
    }
    lastMs = nowMs;

    uint16_t off = (uint16_t)fragIndex * IrrigationProto::FRAG_DATA_MAX;
    if (off + fragLen > curTotalLen)
        return Add::INVALID;
    memcpy(buf + off, data, fragLen);
    receivedMask |= (uint16_t)(1u << fragIndex);

    uint16_t full = (uint16_t)((1u << curFragCount) - 1);
    if (receivedMask != full)
        return Add::ACCEPTED;

    if (IrrigationProto::crc32(buf, curTotalLen) != curCrc) {
        reset();
        return Add::BAD_CRC;
    }
    return Add::COMPLETE;
}
