#pragma once

#include <stdint.h>
#include <stddef.h>

struct Program {
    uint8_t id = 0; // 0 = vazio
    bool enabled = true;
    uint8_t daysMask = 0;    // bit0 = domingo … bit6 = sábado
    uint16_t startMinute = 0; // minuto do dia local (0..1439)
    uint8_t stepCount = 0;
    struct Step {
        uint8_t zoneId = 0;
        uint16_t durationMin = 0;
    } steps[8];
};

struct SchedAction {
    enum class Type : uint8_t { NONE, OPEN, CLOSE } type = Type::NONE;
    uint8_t zoneId = 0;
    uint16_t durationS = 0;
};

class ProgramScheduler {
  public:
    static constexpr size_t MAX_PROGRAMS = 8;
    static constexpr uint32_t MAGIC = 0x49505231; // "IPR1"

    bool upsert(const Program &p);
    bool removeById(uint8_t id);
    size_t count() const;
    const Program *programAt(size_t index) const; // index-ésimo programa ocupado; nullptr se >= count()
    // Chamar 1×/s com hora local válida (epochLocalSecs != 0). Devolve UMA ação
    // por chamada; chamar de novo no mesmo segundo até NONE.
    SchedAction tick(uint32_t epochLocalSecs);
    bool running() const { return activeProgram != 0; }
    uint8_t runningProgramId() const { return activeProgram; } // id do programa em execução (0 = nenhum)
    void abort(); // glue fecha a zona corrente antes (usa currentZone())
    uint8_t currentZone() const { return running() ? curZone : 0; }
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);

  private:
    Program programs[MAX_PROGRAMS] = {};
    uint8_t activeProgram = 0;
    uint8_t curStep = 0;
    uint8_t curZone = 0;
    uint32_t stepEndSecs = 0;
    uint32_t lastFireKey = 0;
    bool pendingOpenNext = false;
};
