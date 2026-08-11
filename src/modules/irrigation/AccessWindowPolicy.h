#pragma once
#include "modules/irrigation/IrrigationSettings.h"
#include <stdint.h>

// Política pura da janela de acesso (Portal AP + BLE) nos nós de campo.
// Concentra a decisão SEM dependências de Arduino/WiFi/BLE, para permitir teste
// nativo. Ver spec docs/superpowers/specs/2026-08-11-irrigacao-janela-acesso-ble-portal-design.md.
namespace AccessWindowPolicy
{

// Papel elegível ao regime de janela: estação/repetidor provisionados.
// Nó de fábrica / não provisionado mantém BLE + portal vivos (sem teardown).
inline bool eligible(IrrigationRole role, bool provisioned)
{
    return provisioned && (role == IrrigationRole::ESTACAO || role == IrrigationRole::REPETIDOR);
}

// Transição OPEN->CLOSED numa etapa: precisa derrubar o BLE agora?
// Só na borda de fechamento, num nó elegível, e uma única vez por boot.
inline bool shouldTearDownBle(bool eligible, bool windowOpenNow, bool windowWasOpen, bool bleAlreadyReleased)
{
    return eligible && windowWasOpen && !windowOpenNow && !bleAlreadyReleased;
}

enum class ButtonAction : uint8_t { NONE, REOPEN_LIVE, REBOOT_TO_REOPEN };

// SHORT press num nó elegível:
//  - não elegível   -> NONE
//  - BLE ainda vivo -> REOPEN_LIVE (requestOpen, sem reboot)
//  - BLE já liberado -> REBOOT_TO_REOPEN (boot novo = janela nova)
inline ButtonAction buttonShortAction(bool eligible, bool bleReleased)
{
    if (!eligible)
        return ButtonAction::NONE;
    return bleReleased ? ButtonAction::REBOOT_TO_REOPEN : ButtonAction::REOPEN_LIVE;
}

} // namespace AccessWindowPolicy
