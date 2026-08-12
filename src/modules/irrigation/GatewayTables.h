#pragma once

#include <stddef.h>
#include <stdint.h>

struct Zone {
    uint8_t id = 0;          // 0 = slot vazio
    char name[16] = {0};
    uint32_t node = 0;
    uint8_t tipo = 0;        // 0 = valvula, 1 = gpo
    uint8_t index = 0;       // índice da saída na estação
    uint16_t maxMin = 120;
    uint16_t padraoMin = 20;
    int8_t fonteInput = -1;  // 0-3 = entrada espelho do gateway; -1 = nenhuma
    uint8_t fonteEnabled = 1; // 1 = associação de espelho ativa; 0 = pausada (fonteInput preservado)
};

// Uma zona cujo node é o próprio nó do gateway aciona a saída LOCAL (sem rádio).
// node==0 (slot vazio / broadcast) nunca conta como local.
inline bool isLocalTarget(uint32_t node, uint32_t selfNode)
{
    return node != 0 && node == selfNode;
}

class ZoneTable {
  public:
    static constexpr size_t MAX = 24;
    static constexpr uint32_t MAGIC = 0x495A4E32; // "IZN2"
    bool upsert(const Zone &z);          // por id (1..255); false = cheia
    bool removeById(uint8_t id);
    const Zone *byId(uint8_t id) const;  // nullptr = ausente
    const Zone *byFonte(int8_t input) const;
    const Zone *zoneAt(size_t index) const; // index-ésima zona ocupada; nullptr se >= count()
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n); // falha => tabela vazia

  private:
    Zone zones[MAX];
};

struct StationEntry {
    uint32_t node = 0;       // 0 = slot vazio
    char name[16] = {0};
    uint32_t desiredEpoch = 0;
    uint8_t blob[208] = {0}; // config v8 desejada (push §5.4)
    uint8_t retries = 3;
    uint16_t silencioAlertaMin = 35;
    int32_t lat = 0, lon = 0; // graus * 1e-5 (WGS84, §8.8)
};

class StationRegistry {
  public:
    static constexpr size_t MAX = 16;
    static constexpr uint32_t MAGIC = 0x49535431; // "IST1"
    // Tamanho serializado por entrada e teto do buffer completo (usado pelo IrrigationModule
    // ao ler/gravar a tabela). node(4)+name(16)+desiredEpoch(4)+blob(208)+retries(1)+
    // silencioAlertaMin(2)+lat(4)+lon(4) = 243.
    static constexpr size_t SERIALIZED_ENTRY = 243;
    static constexpr size_t SERIALIZED_MAX = 6 + MAX * SERIALIZED_ENTRY;
    bool upsert(const StationEntry &e);  // por node; false = cheio
    bool removeByNode(uint32_t node);
    const StationEntry *byNode(uint32_t node) const;
    StationEntry *mutableByNode(uint32_t node);
    const StationEntry *nodeAt(size_t index) const; // index-ésima entrada ocupada; nullptr se >= count()
    size_t count() const;
    void adoptConfig(uint32_t node, const uint8_t *blobData, size_t blobLen, uint32_t epoch); // regra do maior epoch; migra p/ v7 canônico
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);

  private:
    StationEntry stations[MAX];
};
