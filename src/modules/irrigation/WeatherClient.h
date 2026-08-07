#pragma once
#include "modules/irrigation/WeatherConfig.h"
#include "modules/irrigation/WeatherForecast.h"
#include <stdint.h>

// Cliente de rede da Open-Meteo. Sem estado persistido: preenche WeatherCache.
// Todo o corpo de rede é guardado por ARCH_ESP32; no nativo vira no-op.
class WeatherClient {
  public:
    // Chamar ~1×/s com hora local válida. Dispara poll nas horas configuradas
    // (pollHourA/B), uma vez por ocorrência, e um poll único ~30s após WiFi up.
    // Retorna true se atualizou o cache neste tick.
    bool tick(const WeatherConfig &cfg, uint32_t nowLocalSecs, bool staUp, WeatherCache &out);
    // Força um poll imediato (usado pelo botão "Atualizar"). Retorna sucesso.
    bool pollNow(const WeatherConfig &cfg, uint32_t nowEpoch, WeatherCache &out);

  private:
    uint32_t lastPollKey = 0;   // (dia*100 + hora) do último poll agendado
    bool seededAfterBoot = false;
    uint32_t staUpSinceMs = 0;
};
