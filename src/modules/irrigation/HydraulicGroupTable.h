#pragma once
#include <stddef.h>
#include <stdint.h>

// §8.13 — grupo hidráulico (bomba + válvulas). Orquestração 100% gateway;
// a bomba é uma zona gpo comum (bombaZoneId), sem ABI nova de estação.
struct HydraulicGroup {
    uint8_t  id = 0;               // 0 = slot vazio
    char     name[16] = {0};
    uint8_t  bombaZoneId = 0;      // zona gpo da bomba; 0 = grupo SEM bomba (== §8.10 simultaneidade)
    uint8_t  zoneIds[8] = {0};     // válvulas membro
    uint8_t  zoneCount = 0;
    uint8_t  minOpen = 1;          // min_abertas_com_bomba
    uint8_t  maxOpen = 1;          // max_abertas (0 = sem teto)
    uint8_t  transicao = 0;        // 0 = abrir_antes_de_fechar, 1 = fechar_antes_de_abrir
    uint16_t overlapS = 10;        // sobreposicao_s
    uint16_t startAfterOpenS = 5;  // partida_apos_abrir_s
    uint16_t stopBeforeCloseS = 8; // parar_antes_de_fechar_s
    uint16_t minRunMin = 5;        // funcionamento_min_min
    uint8_t  maxStartsHour = 6;    // max_partidas_hora
};

class HydraulicGroupTable {
  public:
    static constexpr size_t MAX = 8;
    static constexpr uint32_t MAGIC = 0x49484731; // "IHG1"

    bool upsert(const HydraulicGroup &g);           // valida; false = cheia/inválida
    bool removeById(uint8_t id);
    const HydraulicGroup *byId(uint8_t id) const;       // nullptr = ausente
    const HydraulicGroup *byZone(uint8_t zoneId) const; // grupo que contém a zona-membro
    const HydraulicGroup *byPumpZone(uint8_t zoneId) const; // grupo cuja bomba é essa zona
    const HydraulicGroup *groupAt(size_t index) const;  // index-ésimo grupo ocupado
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);     // falha => tabela vazia

  private:
    HydraulicGroup groups[MAX];
    bool zoneUsedByOther(uint8_t zoneId, uint8_t exceptId) const;
    bool valid(const HydraulicGroup &g) const;
};
