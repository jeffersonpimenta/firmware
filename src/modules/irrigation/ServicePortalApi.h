#pragma once
#include "modules/irrigation/IServiceLogReader.h"
#include "modules/irrigation/IrrigationSettings.h"
#include "modules/irrigation/IrrigationWebApi.h" // JsonWriter/JsonReader/ParseResult (reuso)
#include "modules/irrigation/ServiceBackup.h"     // IrrigationService::LightProfile + scanner estrutural
#include "modules/irrigation/ServiceController.h" // ScanResults/ScanEntry
#include <cstddef>
#include <cstdint>

// Camada PURA do portal do device SERVICO (Fase 8c, §11.8). Sem I/O, sem rádio: recebe dados
// como parâmetro. Espelha PortalApi/IrrigationWebApi. Nativo-testada em test_service_portal.
namespace IrrigationWeb
{

// ── Aba Clientes ─────────────────────────────────────────────────────────────
// [{id,nome,canal,preset,gateway,estacoes,active}] sob {"clients":[…]}.
size_t buildClientList(const IrrigationService::LightProfile *clients, size_t n, const char *activeId, char *buf,
                       size_t cap);
// {"id":"<clientId>"} → idOut. Falha se ausente/vazio.
ParseResult parseSelect(const char *json, size_t len, char *idOut, size_t idCap);

// ── Aba Rede — varredura ─────────────────────────────────────────────────────
// {"nodes":[{node,role,epoch,vbat,fw,lat,lon,snr}]}.
size_t buildScanResults(const ScanResults &scan, char *buf, size_t cap);

// ── Aba Rede — editor de config (settings v5 inteiro) ────────────────────────
// Serializa o blob v5. version/role/configEpoch/boundGateway saem como informativos
// (read-only no editor); parseStationConfig os preserva do `out` de entrada.
size_t buildStationConfig(const IrrigationSettings &s, char *buf, size_t cap);
// Reconstrói os campos editáveis do blob a partir do JSON do editor. PRESERVA
// magic/version/role/boundGateway/configEpoch do `out` (o chamador semeia com o blob lido).
// Tolerante: campo ausente mantém o valor de `out`. Falha só com JSON vazio.
ParseResult parseStationConfig(const char *json, size_t len, IrrigationSettings &out);

// ── Aba Rede — escrita de config (2 rotas §11.6) ─────────────────────────────
enum class SvcRoute : uint8_t { VIA_GATEWAY, DIRECT };
struct NodeConfigReq {
    uint32_t node = 0;
    SvcRoute route = SvcRoute::VIA_GATEWAY;
    IrrigationSettings config; // chamador semeia com o blob lido; parse reescreve os editáveis
};
// {node:"!hex", route:"gateway"|"direct", config:{…}}.
ParseResult parseNodeConfigReq(const char *json, size_t len, NodeConfigReq &out);

// ── Aba Rede — ações por nó ──────────────────────────────────────────────────
enum class SvcAction : uint8_t { NONE, PULSE, ZONE, APPROVE_PAIR, RESYNC };
struct NodeAction {
    uint32_t node = 0;
    SvcAction action = SvcAction::NONE;
    uint8_t valveOrZoneId = 0; // PULSE: valveId 0..7; ZONE: zoneId 1..255
    uint16_t durationS = 0;
    bool open = false; // ZONE: abrir(1)/fechar(0)
};
// {node:"!hex", action:"pulse"|"zone"|"approve_pair"|"resync", …}. "open" como 0/1.
ParseResult parseNodeAction(const char *json, size_t len, NodeAction &out);

// ── Aba Log ──────────────────────────────────────────────────────────────────
// {"log":[ <linha>, <linha>, … ]} — cada linha é um objeto JSON já pronto (raw).
size_t buildServiceLog(IServiceLogReader &reader, size_t maxLines, char *buf, size_t cap);

} // namespace IrrigationWeb
