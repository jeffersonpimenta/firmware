#include "SeqTable.h"

bool SeqTable::checkAndUpdate(uint32_t sender, uint32_t seq)
{
    Entry *empty = nullptr;
    Entry *lowest = &entries[0];
    for (auto &e : entries) {
        if (e.node == sender) {
            if (seq <= e.seq)
                return false;
            e.seq = seq;
            return true;
        }
        if (e.node == 0 && !empty)
            empty = &e;
        if (e.seq < lowest->seq)
            lowest = &e;
    }
    Entry *slot = empty ? empty : lowest;
    slot->node = sender;
    slot->seq = seq;
    return true;
}

uint32_t SeqTable::lastSeq(uint32_t sender) const
{
    for (const auto &e : entries)
        if (e.node == sender)
            return e.seq;
    return 0;
}
