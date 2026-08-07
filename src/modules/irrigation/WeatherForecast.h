#pragma once
#include <stdint.h>

// Snapshot do último forecast (RAM, não persistido). Unidades inteiras.
struct WeatherCache {
    bool     valid = false;
    bool     isMock = false;      // último fetch falhou → exibe "estimativa"
    uint32_t fetchEpoch = 0;      // epoch local do fetch (base do TTL)
    // decisão:
    uint16_t chuvaPrevista12hCenti = 0; // Σ precip horas i+1..i+12 (centi-mm)
    uint8_t  probChuvaPct = 0;          // max prob horas i..i+12
    // exibição (espelha o card do mockup):
    uint16_t chuvaAcum24hCenti = 0;
    int16_t  tempAtualCenti = 0;
    uint8_t  umidadeRelPct = 0;
    uint8_t  umidadeSoloPct = 0;
    int16_t  tempMinCenti = 0;
    int16_t  tempMaxCenti = 0;
    uint16_t ventoRajadaCenti = 0;
    uint16_t et0Centi = 0;
};

struct WeatherVerdict {
    bool    suppress = false;
    uint8_t ruleId = 0; // regra que causou a supressão (0 = nenhuma)
};
