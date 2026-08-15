#include "IrrigationAirtime.h"
#include "IrrigationProtocol.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

namespace IrrigationAirtime
{

using namespace IrrigationProto;

uint8_t priorityForType(uint8_t msgType, bool criticalEvent)
{
    switch (msgType) {
    case MSG_CMD_VALVULA:
    case MSG_CMD_GPO:
    case MSG_REMOTE_CMD:
    case MSG_REMOTE_TRIGGER:
        return meshtastic_MeshPacket_Priority_HIGH;
    case MSG_CMD_MAINT:
        return meshtastic_MeshPacket_Priority_ALERT;
    case MSG_ACK:
        return meshtastic_MeshPacket_Priority_RESPONSE;
    case MSG_EVENTO:
        return criticalEvent ? meshtastic_MeshPacket_Priority_ALERT : meshtastic_MeshPacket_Priority_DEFAULT;
    case MSG_HEARTBEAT:
    case MSG_PING_SURVEY:
        return meshtastic_MeshPacket_Priority_BACKGROUND;
    default:
        return meshtastic_MeshPacket_Priority_DEFAULT;
    }
}

bool isGatedType(uint8_t msgType)
{
    return msgType == MSG_HEARTBEAT || msgType == MSG_PING_SURVEY;
}

bool isCriticalEvent(uint8_t evCode)
{
    return evCode == EV_TAMPER;
}

uint8_t hbBackoffFactor(float chUtilPercent)
{
    if (chUtilPercent >= 60.0f)
        return 8;
    if (chUtilPercent >= 40.0f)
        return 4;
    if (chUtilPercent >= 25.0f)
        return 2;
    return 1;
}

uint32_t hbWindowMs(uint32_t effectiveIntervalMs)
{
    uint32_t quarter = effectiveIntervalMs / 4;
    return quarter < 30000u ? quarter : 30000u;
}

uint32_t hbJitterOffsetMs(uint32_t nodeNum, uint32_t epoch, uint32_t windowMs)
{
    if (windowMs == 0)
        return 0;
    // FNV-1a 32-bit sobre (nodeNum ^ epoch): determinístico, boa dispersão.
    uint32_t x = nodeNum ^ epoch;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) {
        h ^= (x & 0xff);
        h *= 16777619u;
        x >>= 8;
    }
    return h % windowMs;
}

} // namespace IrrigationAirtime
