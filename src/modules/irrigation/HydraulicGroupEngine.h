#pragma once
#include <stddef.h>
#include <stdint.h>
#include "modules/irrigation/GatewayTables.h"        // ZoneTable
#include "modules/irrigation/HydraulicGroupTable.h"

struct GroupEmit {
    uint32_t node = 0;
    uint8_t  index = 0;     // índice da saída na estação
    uint8_t  tipo = 0;      // 0 valvula, 1 gpo
    uint8_t  zoneId = 0;
    uint8_t  action = 0;    // 1 abrir, 0 fechar
    uint16_t durationS = 0;
};

class HydraulicGroupEngine {
  public:
    static constexpr size_t MAX_GROUPS = HydraulicGroupTable::MAX; // 8
    static constexpr size_t MAX_ZONES = 8;
    static constexpr uint16_t PUMP_MARGIN_S = 120;
    static constexpr uint16_t PUMP_CEILING_S = 7200; // estação clampa em 120 min
    static constexpr uint32_t START_WINDOW_MS = 3600000u; // janela de max_partidas_hora

    enum class State : uint8_t {
        IDLE, OPENING, START_WAIT, PUMP_WAIT_ACK, RUNNING,
        X_OPEN_WAIT, X_OVERLAP, X_CLOSE_WAIT,
        PUMP_OFF_WAIT, DRAIN, CLOSE_LAST_WAIT, DEFERRED
    };

    enum AlertCode : uint8_t {
        GA_NONE = 0, GA_PUMP_ON, GA_PUMP_OFF,
        GA_OPEN_FAIL_RENEW, GA_OPEN_FAIL_PUMPOFF,
        GA_CLOSE_FAIL, GA_ORDERED_SHUTDOWN,
        GA_DEFER_RATE, GA_DEFER_MINOPEN, GA_REBOOT_RECONCILE
    };
    struct GroupAlert { uint8_t groupId = 0; uint8_t code = 0; uint8_t zoneId = 0; };

    void reset();
    void setDesired(uint8_t groupId, uint8_t zoneId, bool open, uint16_t durationS);
    size_t tick(const HydraulicGroupTable &tbl, const ZoneTable &zones, uint32_t nowMs,
                GroupEmit *out, size_t cap);
    void noteSent(uint32_t node, uint8_t zoneId, uint8_t action, uint32_t seq);
    void onAck(uint32_t node, uint32_t ackedSeq);
    void onCmdFailed(uint32_t node, uint8_t zoneId, uint8_t action);
    void observeActual(uint8_t groupId, uint8_t zoneId, bool open);
    bool takeAlert(GroupAlert &out);

    // Observabilidade p/ testes.
    State stateOf(uint8_t groupId) const;
    bool pumpOn(uint8_t groupId) const;

  private:
    struct ZoneRt {
        uint8_t zoneId = 0;          // 0 = slot livre
        bool wanted = false;
        uint16_t wantDurS = 0;
        bool confirmed = false;      // aberta confirmada
        uint32_t localExpiresMs = 0; // deadline do timer local (0 = fechada)
    };
    struct PendCmd {                 // um comando emitido aguardando ACK
        bool inUse = false;
        uint32_t node = 0, seq = 0;
        uint8_t zoneId = 0, action = 0;
    };
    struct GroupRt {
        State state = State::IDLE;
        bool pump = false;
        uint8_t curZone = 0;
        uint8_t nextZone = 0;
        uint32_t waitStartMs = 0;
        uint32_t lastStartMs = 0;    // última partida da bomba (bridging)
        uint32_t startRing[8] = {0}; // timestamps de partida (max_partidas_hora)
        uint8_t  startCount = 0;     // partidas na janela corrente
        PendCmd pend;                // 1 comando pendente por grupo (sequencial)
        ZoneRt zones[MAX_ZONES];
    };

    GroupRt rt[MAX_GROUPS];
    GroupAlert alertRing[16];
    size_t aHead = 0, aTail = 0, aCount = 0;

    GroupRt &rtOf(uint8_t groupId);            // groupId 1..8 -> rt[groupId-1]
    ZoneRt *findZone(GroupRt &g, uint8_t zoneId);
    ZoneRt *ensureZone(GroupRt &g, uint8_t zoneId);
    void pushAlert(uint8_t groupId, uint8_t code, uint8_t zoneId);
    // Preenche node/index/tipo a partir da ZoneTable; retorna false se zona ausente.
    bool resolve(const ZoneTable &zones, uint8_t zoneId, GroupEmit &e) const;
    uint16_t pumpDur(uint16_t zoneDurS) const;
    size_t confirmedCount(const GroupRt &g) const;
    uint8_t pruneStarts(GroupRt &g, uint32_t nowMs) const; // remove partidas > 1h, devolve count
};
