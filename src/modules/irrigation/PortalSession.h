#pragma once
#include <stdint.h>

// Ciclo de vida do AP do captive portal (spec §7.2, §8.7). Timer puro, sem Arduino/WiFi:
// requestOpen() abre por PORTAL_TIMEOUT_MS; cada tick com cliente presente renova a atividade;
// sem cliente por PORTAL_TIMEOUT_MS o AP fecha. A cola (PortalAp) consome apShouldBeUp().
class PortalSession
{
  public:
    static constexpr uint32_t PORTAL_TIMEOUT_MS = 10u * 60u * 1000u; // 10 min (spec §7.2)

    enum class State : uint8_t { CLOSED = 0, OPEN = 1 };

    void requestOpen(uint32_t nowMs);                // botão / auto-open de fábrica
    void noteClient(uint32_t nowMs, bool anyClient); // renova a atividade enquanto houver cliente
    void tick(uint32_t nowMs);                        // fecha por inatividade
    bool apShouldBeUp() const { return _state == State::OPEN; }
    uint32_t secondsLeft(uint32_t nowMs) const;

  private:
    State _state = State::CLOSED;
    uint32_t _lastActivityMs = 0;
};
