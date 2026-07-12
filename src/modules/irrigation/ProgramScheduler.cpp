#include "ProgramScheduler.h"
#include <string.h>

namespace
{
constexpr size_t PROG_ENTRY = 1 + 1 + 1 + 2 + 1 + 8 * 3; // 30 B

// serialização compartilhada: magic(4)+ver(1)+count(1)+entries
size_t writeHeader(uint8_t *buf, size_t cap, uint32_t magic, uint8_t count, size_t entrySize)
{
    size_t need = 6 + (size_t)count * entrySize;
    if (cap < need)
        return 0;
    memcpy(buf, &magic, 4);
    buf[4] = 1;
    buf[5] = count;
    return need;
}

bool checkHeader(const uint8_t *buf, size_t n, uint32_t magic, size_t entrySize, uint8_t maxCount, uint8_t &countOut)
{
    if (n < 6)
        return false;
    uint32_t m;
    memcpy(&m, buf, 4);
    if (m != magic || buf[4] != 1)
        return false;
    countOut = buf[5];
    return countOut <= maxCount && n == 6 + (size_t)countOut * entrySize;
}
} // namespace

bool ProgramScheduler::upsert(const Program &p)
{
    if (p.id == 0 || p.stepCount == 0 || p.stepCount > 8)
        return false;

    // Clamp durationMin to 720 (12 hours)
    Program clamped = p;
    for (uint8_t i = 0; i < clamped.stepCount; i++) {
        if (clamped.steps[i].durationMin > 720)
            clamped.steps[i].durationMin = 720;
    }

    for (auto &s : programs)
        if (s.id == clamped.id) {
            if (s.id == activeProgram)
                abort(); // trocar passos sob execução: aborta (glue fecha a zona corrente)
            s = clamped;
            return true;
        }
    for (auto &s : programs)
        if (s.id == 0) {
            s = clamped;
            return true;
        }
    return false;
}

bool ProgramScheduler::removeById(uint8_t id)
{
    for (auto &s : programs)
        if (s.id == id && id != 0) {
            if (activeProgram == id)
                abort();
            s = Program{};
            s.id = 0;
            return true;
        }
    return false;
}

size_t ProgramScheduler::count() const
{
    size_t c = 0;
    for (auto &s : programs)
        if (s.id != 0)
            c++;
    return c;
}

void ProgramScheduler::abort()
{
    activeProgram = 0;
    curStep = 0;
    curZone = 0;
    pendingOpenNext = false;
    // lastFireKey fica: o mesmo minuto não pode redisparar. Não "consertar".
}

SchedAction ProgramScheduler::tick(uint32_t epochLocalSecs)
{
    SchedAction none;
    if (epochLocalSecs == 0)
        return none;

    if (activeProgram != 0) {
        const Program *p = nullptr;
        for (auto &s : programs)
            if (s.id == activeProgram)
                p = &s;
        if (!p) {
            abort();
            return none;
        }
        if (pendingOpenNext) { // segunda metade da transição CLOSE→OPEN
            pendingOpenNext = false;
            curZone = p->steps[curStep].zoneId;
            stepEndSecs = epochLocalSecs + (uint32_t)p->steps[curStep].durationMin * 60;
            return {SchedAction::Type::OPEN, curZone, (uint16_t)(p->steps[curStep].durationMin * 60)};
        }
        if (epochLocalSecs >= stepEndSecs) {
            SchedAction close{SchedAction::Type::CLOSE, curZone, 0};
            if (curStep + 1 < p->stepCount) {
                curStep++;
                pendingOpenNext = true;
            } else {
                abort();
            }
            return close;
        }
        return none;
    }

    uint32_t minuteKey = epochLocalSecs / 60;
    if (minuteKey == lastFireKey)
        return none;
    uint8_t weekday = (uint8_t)(((epochLocalSecs / 86400) + 4) % 7);
    uint16_t minuteOfDay = (uint16_t)((epochLocalSecs % 86400) / 60);
    for (auto &p : programs) {
        if (p.id == 0 || !p.enabled || p.stepCount == 0)
            continue;
        if (!(p.daysMask & (1u << weekday)) || p.startMinute != minuteOfDay)
            continue;
        lastFireKey = minuteKey;
        activeProgram = p.id;
        curStep = 0;
        curZone = p.steps[0].zoneId;
        stepEndSecs = epochLocalSecs + (uint32_t)p.steps[0].durationMin * 60;
        return {SchedAction::Type::OPEN, curZone, (uint16_t)(p.steps[0].durationMin * 60)};
    }
    return none;
}

size_t ProgramScheduler::serialize(uint8_t *buf, size_t cap) const
{
    uint8_t cnt = (uint8_t)count();
    size_t need = writeHeader(buf, cap, MAGIC, cnt, PROG_ENTRY);
    if (!need)
        return 0;
    size_t off = 6;
    for (auto &p : programs) {
        if (p.id == 0)
            continue;
        buf[off] = p.id;
        buf[off + 1] = p.enabled ? 1 : 0;
        buf[off + 2] = p.daysMask;
        memcpy(buf + off + 3, &p.startMinute, 2);
        buf[off + 5] = p.stepCount;
        for (uint8_t i = 0; i < 8; i++) {
            buf[off + 6 + i * 3] = p.steps[i].zoneId;
            memcpy(buf + off + 7 + i * 3, &p.steps[i].durationMin, 2);
        }
        off += PROG_ENTRY;
    }
    return need;
}

bool ProgramScheduler::deserialize(const uint8_t *buf, size_t n)
{
    for (auto &s : programs)
        s = Program{};
    uint8_t cnt;
    if (!checkHeader(buf, n, MAGIC, PROG_ENTRY, (uint8_t)MAX_PROGRAMS, cnt))
        return false;
    size_t off = 6;
    for (uint8_t i = 0; i < cnt; i++, off += PROG_ENTRY) {
        Program p;
        p.id = buf[off];
        p.enabled = buf[off + 1] != 0;
        p.daysMask = buf[off + 2];
        memcpy(&p.startMinute, buf + off + 3, 2);
        p.stepCount = buf[off + 5];
        for (uint8_t j = 0; j < 8; j++) {
            p.steps[j].zoneId = buf[off + 6 + j * 3];
            memcpy(&p.steps[j].durationMin, buf + off + 7 + j * 3, 2);
        }
        programs[i] = p;
    }
    return true;
}
