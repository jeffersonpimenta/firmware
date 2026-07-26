#pragma once
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/HydraulicGroupTable.h"
#include "modules/irrigation/InterlockTable.h"
#include "modules/irrigation/IrrigationProtocol.h"
#include "modules/irrigation/ProgramScheduler.h"
#include "modules/irrigation/StationMonitor.h" // AlertCenter/Alert p/ buildAlerts
#include "modules/irrigation/SurveyLog.h"
#include <cstddef>
#include <cstdint>

namespace IrrigationWeb
{

// Writer JSON mínimo: escreve em um buffer fixo do chamador. overflow() vira true e
// todas as escritas subsequentes viram no-op; done() devolve bytes escritos ou 0 se
// houve overflow. Sem alocação, sem dependência externa.
class JsonWriter
{
  public:
    JsonWriter(char *buf, size_t cap) : _b(buf), _cap(cap) {}
    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(const char *k);          // escreve "k": e arma vírgula pós-valor
    void str(const char *v);          // valor string (com escape básico)
    void num(int64_t v);              // valor inteiro
    void boolean(bool v);
    void raw(const char *v);          // valor já-JSON (ex.: objeto aninhado montado à parte)
    void keyStr(const char *k, const char *v) { key(k); str(v); }
    void keyNum(const char *k, int64_t v) { key(k); num(v); }
    void keyBool(const char *k, bool v) { key(k); boolean(v); }
    bool overflow() const { return _ovf; }
    size_t done();                    // fecha nada; devolve _len (0 se overflow)

  private:
    void putc_(char c);
    void puts_(const char *s);
    void sep_();                      // vírgula entre itens conforme estado
    char *_b;
    size_t _cap;
    size_t _len = 0;
    bool _ovf = false;
    bool _needComma = false;
};

enum class SyncState : uint8_t { SINCRONIZADA = 0, PENDENTE = 1, INALCANCAVEL = 2 };
SyncState computeSync(uint32_t desiredEpoch, uint32_t reportedEpoch, bool silent);
const char *syncLabel(SyncState s); // "sincronizada" | "pendente" | "inalcancavel"

struct OverviewCtx {
    bool hasRtc = false;
    uint16_t stationCount = 0;
    uint8_t running = 0; // 0 = ocioso, 1 = executando
    uint16_t alertCount = 0;
    bool pairingPending = false;
    uint32_t pairingNodeId = 0;
    uint16_t pairingSecondsLeft = 0;
    uint8_t runningZoneId = 0;
    uint16_t runningRemainMin = 0;
};
size_t buildOverview(const OverviewCtx &ctx, char *buf, size_t cap);

// Alertas não reconhecidos (§8.1–§8.3): serializa os alertas do AlertCenter com atMs > ackMs.
// ageS = (nowMs - atMs)/1000. JSON: [{type,node,arg,ageS}]. "[]" se nada pendente.
size_t buildAlerts(const AlertCenter &ac, uint32_t nowMs, uint32_t ackMs, char *buf, size_t cap);

struct StationView {
    uint32_t node = 0;
    const char *name = "";
    SyncState sync = SyncState::SINCRONIZADA;
    uint32_t secsSinceHeard = 0;
    uint16_t vbatCentiV = 0;
    uint16_t vpanelCentiV = 0;
    int8_t snrQuarterDb = 0;
    uint16_t rebootCount = 0;
    uint8_t flags = 0; // HbFlags: tamper/safe/hibernation
    int32_t lat = 0, lon = 0;
};
size_t buildStations(const StationView *views, size_t n, char *buf, size_t cap);

size_t buildZones(const ZoneTable &zones, char *buf, size_t cap);
size_t buildPrograms(const ProgramScheduler &sched, char *buf, size_t cap);

struct ParseError {
    char msg[48];
};
struct ParseResult {
    bool ok = true;
    uint8_t errorCount = 0;
    ParseError errors[4];
    void fail(const char *m);
};

// Reader de objeto JSON plano: só o nível superior, valores número/string/bool.
// Sem aninhamento (arrays/objetos são ignorados pelas getters escalares). Suficiente
// para os corpos que o painel envia. Para programas (com array), ver Task 7.
class JsonReader
{
  public:
    JsonReader(const char *json, size_t len) : _j(json), _len(len) {}
    bool getInt(const char *key, int64_t &out) const;
    bool getStr(const char *key, char *out, size_t cap) const;
    bool getBool(const char *key, bool &out) const;

  private:
    const char *findValue(const char *key) const; // aponta pro 1º char do valor, ou nullptr
    const char *_j;
    size_t _len;
};

ParseResult parseZoneUpsert(const char *json, size_t len, Zone &out);
ParseResult parseZoneDelete(const char *json, size_t len, uint8_t &outId);

ParseResult parseProgramUpsert(const char *json, size_t len, Program &out);
ParseResult parseProgramToggle(const char *json, size_t len, uint8_t &outId, bool &outEnabled);
ParseResult parseProgramDelete(const char *json, size_t len, uint8_t &outId);

enum class CmdKind : uint8_t { NONE, PULSE_TEST, OPEN, CLOSE, ACK_ALERT, APPROVE_PAIRING };
struct WebCommand {
    CmdKind kind = CmdKind::NONE;
    uint8_t zoneId = 0;
    uint16_t durationS = 0;
    uint32_t node = 0;
    // ACK_ALERT: identidade do alerta clicado (node acima + estes 3). atMs==0 ⇒ reconhecer todos (legado).
    uint8_t alertType = 0;
    uint32_t arg = 0;
    uint32_t atMs = 0;
};
ParseResult parseCommand(const char *json, size_t len, WebCommand &out);

// ── Fase 6b — intertravamentos ───────────────────────────────────────────────

size_t buildInterlocks(const InterlockTable &tbl, char *buf, size_t cap);
ParseResult parseInterlockUpsert(const char *json, size_t len, InterlockRule &out);
ParseResult parseInterlockDelete(const char *json, size_t len, uint8_t &outId);

// ── Fase 6b — sensores do gateway ────────────────────────────────────────────

// Item de sensor individual para exibição no painel.
struct GwSensorItem {
    uint8_t idx;           // 0..HB_MAX_SENSORS-1
    uint8_t tipo;          // tipo de sensor (campo raw do protocolo)
    int16_t valueCenti;    // valor em centésimos da unidade
    const char *name;      // ponteiro p/ nome — "" = sem nome configurado
};

// Vista de uma estação com seus sensores, pronta para serialização.
struct GwStationSensors {
    uint32_t node;
    const char *stationName; // "" = sem nome
    bool tamper;
    uint8_t count;           // quantos itens válidos em `itens`
    GwSensorItem itens[IrrigationProto::HB_MAX_SENSORS];
};

// Serializa array de estações com sensores.
// JSON: [{node,nome,tamper,sensores:[{idx,tipo,valor,nome},...]}]
size_t buildSensorsGateway(const GwStationSensors *views, size_t n, char *buf, size_t cap);

// ── Fase 6b — janela de manutenção / nomes de sensor ─────────────────────────

// Parse {node, minutes}. minutes validado em 0..1440.
ParseResult parseMaintWindow(const char *json, size_t len, uint32_t &outNode, uint16_t &outMinutes);

// Parse {node, sensor, nome}. sensor validado em 0..3; nome truncado a nameCap-1.
ParseResult parseSensorName(const char *json, size_t len,
                            uint32_t &outNode, uint8_t &outIdx,
                            char *outName, size_t nameCap);

// ── Fase 7b — grupos hidráulicos ─────────────────────────────────────────────

ParseResult parseGroupUpsert(const char *json, size_t len, HydraulicGroup &out);
ParseResult parseGroupDelete(const char *json, size_t len, uint8_t &outId);
// {id, acao:"abrir"|"fechar", durationS?}. durationS omitido => 0 (módulo aplica default).
ParseResult parseGroupCommand(const char *json, size_t len, uint8_t &outId, bool &outOpen, uint16_t &outDurationS);

// Validação semântica (precisa da ZoneTable). true=ok; senão preenche err (>=48 bytes).
bool validateGroupZones(const HydraulicGroup &g, const ZoneTable &zones, char *err, size_t errCap);

const char *groupStateLabel(uint8_t state);
size_t buildGroups(const HydraulicGroupTable &tbl, char *buf, size_t cap);

struct GroupStatusView {
    uint8_t id = 0;
    const char *name = "";
    uint8_t state = 0;     // HydraulicGroupEngine::State
    bool pump = false;
    uint8_t curZone = 0;
    uint8_t openCount = 0;
};
size_t buildGroupsStatus(const GroupStatusView *views, size_t n, char *buf, size_t cap);

// ── Fase 8d — site survey (§8.5) ─────────────────────────────────────────────
// Serializa pontos de survey p/ a tabela Cobertura do painel. idadeS = nowS - uptimeS.
size_t buildSurvey(const SurveyPoint *pts, size_t n, uint32_t nowS, char *buf, size_t cap);

struct SurveyStartReq {
    uint16_t intervalS = 5;
    uint16_t timeoutS = 300;
    int32_t latE7 = 0, lonE7 = 0;
    bool hasCoord = false;
};
// {intervalS?, timeoutS?, lat?, lon?}. Defaults 5 s / 300 s; clamp [1,3600].
// hasCoord = lat E lon presentes. Nunca falha (pr.ok sempre true).
ParseResult parseSurveyStart(const char *json, size_t len, SurveyStartReq &out);

} // namespace IrrigationWeb
