#pragma once
#include <stdint.h>

// Decisões puras de gerenciamento de airtime do módulo de irrigação.
// Sem estado, sem dependência de AirTime real — testáveis em nativo.
namespace IrrigationAirtime
{

// Prioridade meshtastic_MeshPacket_Priority por tipo IrrigationProto::MsgType.
// criticalEvent só é consultado quando msgType == MSG_EVENTO.
uint8_t priorityForType(uint8_t msgType, bool criticalEvent);

// true → o pacote cede airtime sob congestionamento (HEARTBEAT, PING_SURVEY).
bool isGatedType(uint8_t msgType);

// Evento que escala para prioridade ALERT (tamper). Demais = rotina.
bool isCriticalEvent(uint8_t evCode);

// Multiplicador do intervalo de heartbeat conforme utilização do canal (%).
// <25→1, [25,40)→2, [40,60)→4, >=60→8.
uint8_t hbBackoffFactor(float chUtilPercent);

// Janela de jitter: min(effectiveIntervalMs/4, 30000).
uint32_t hbWindowMs(uint32_t effectiveIntervalMs);

// Offset determinístico de jitter em [0, windowMs). Muda com epoch. 0 se windowMs==0.
uint32_t hbJitterOffsetMs(uint32_t nodeNum, uint32_t epoch, uint32_t windowMs);

} // namespace IrrigationAirtime
