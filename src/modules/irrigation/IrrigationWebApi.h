#pragma once
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

} // namespace IrrigationWeb
