# Horário (gateway) + relógio nos nós — design

**Branch:** `sistema-irrigacao`  ·  **Data:** 2026-08-06
**Fase:** 8b (segue a 8a — WiFi STA + mDNS já entregues)

## Objetivo

Portar a página **Horário** do mockup (`Irrigacao Mobile.dc.html`, aba "Mais")
para o painel do gateway, e incluir o **relógio na página principal** dos portais
de nó (estação e repetidor). A base de horário puxa da internet (NTP, automático
via WiFi STA) **ou** é configurada manualmente. Fuso horário selecionável.

## Contexto existente (não reconstruir)

- **NTP já funciona no firmware.** `src/mesh/wifi/WiFiAPClient.cpp` mantém um
  `NTPClient` sobre `config.network.ntp_server`; a cada 12 h (WiFi conectado)
  chama `perhapsSetRTC(RTCQualityNTP, &tv)`. O gateway (fase 8a) já roda WiFi STA
  sempre-ligado → **a hora da internet já flui automaticamente quando há WiFi.**
- **Fuso.** `config.device.tzdef` guarda a string POSIX TZ; `main.cpp:978` aplica
  via `setenv("TZ", tzdef, 1)` no boot. Gravável em runtime + `tzset()`.
- **Ajuste manual.** `perhapsSetRTC(RTCQuality, const struct timeval*, bool forceUpdate)`
  (`src/gps/RTC.cpp:229`) permite setar o relógio. `forceUpdate=true` sobrepõe.
- **Qualidade / fonte.** `getRTCQuality()` distingue `RTCQualityNTP` (3) de
  `RTCQualityDevice` (1). A **fonte de hora é derivável** — nenhum flag persistido novo.
- **Sync por dispositivo.** `SyncState`/epoch por estação já existe
  (`IrrigationWebApi.cpp` `computeSync`/`syncLabel`); reaproveitado no card
  "Sincronização por dispositivo".
- **Rotas web** são registradas em `IrrigationWebEndpoints.cpp` via
  `server->registerNode(new ResourceNode(path, method, &handler))`.
- **Painel gateway** (`data/irrigacao/app.js`) usa mapa `RENDER` + `SECTION_LABELS`
  + `renderMais()`; sub-telas via `showSub()`. Cabeçalho tem `#syncChip`.
- **Portal nó** (`data/irrigacao/portal/app.js`) `renderNode()` monta o card
  "Este nó" para estação e repetidor.

## Decisões (confirmadas com o usuário)

1. **Backend firmware completo** nesta fase (endpoints reais, não só UI/mock).
2. **Fuso = lista de presets → POSIX TZ** gravada em `config.device.tzdef`.
3. **Relógio no nó = só exibição** (hora vinda da malha); sem config de hora no nó.
4. **"Sincronizar agora" = disparo NTP real** (só quando WiFi STA conectado).
5. **Badge de relógio no cabeçalho** do gateway, clicável → abre Horário.
6. **"Fonte de hora" reflete a realidade**, sem suprimir NTP (ver Simplificação).

## Arquitetura

Mesmo padrão das fases anteriores: builders de JSON/view-model puros em
`IrrigationWebApi.*`, cola de estado em `IrrigationModule.cpp`, rotas em
`IrrigationWebEndpoints.cpp`, UI em `data/irrigacao/`. **Sem mudança de ABI de
estação** (nada novo persiste no blob de config de estação).

### Componentes

| Unidade | Responsabilidade | Interface | Depende de |
|---|---|---|---|
| `buildTimeStatus()` (novo, WebApi) | Serializa estado de hora do gateway | `TimeCtx → JSON` | — (dados injetados) |
| `IrrigationModule` glue | Coleta `TimeCtx`, aplica set manual / tz / sync-NTP | métodos `gwTime*` | `RTC.cpp`, `config`, WiFi helper |
| endpoints `time` / `timezone` | HTTP GET/POST | ResourceNode | WebApi + módulo |
| `TZ_PRESETS` (tabela) | label ↔ POSIX TZ | array const (C++ e JS) | — |
| `renderHorario()` (gateway JS) | 4 cards da página Horário | `/api/irrigation/time` + `/stations` | fetch helpers |
| header time badge (gateway JS) | hora no topo, clique→Horário | `#timeBadge` | `/time` |
| node-state clock (portal) | linha "Relógio" no card do nó | campo `nowEpoch`/`hasTime` | `getValidTime` |

## Backend — endpoints novos

### `GET /api/irrigation/time`  (GATEWAY-only)
Resposta:
```json
{
  "nowEpoch": 1754500320,   // getValidTime(RTCQualityDevice,true); 0 se sem hora
  "hasRtc": true,
  "source": "ntp",          // "ntp" | "manual" | "none"  (derivado de getRTCQuality + staUp)
  "quality": 3,             // getRTCQuality() cru
  "ntpServer": "pool.ntp.org",  // config.network.ntp_server
  "lastSyncS": 120,         // segundos desde o último set NTP (ou -1)
  "tz": "<-03>3",           // config.device.tzdef ("" => UTC)
  "tzLabel": "America/Sao_Paulo", // match no preset, senão "Personalizado"
  "staUp": true             // WiFi STA conectado (pra mostrar/ocultar "Sincronizar agora")
}
```
`source`: `NTP` se `getRTCQuality()>=RTCQualityNTP`; `manual` se `>=RTCQualityDevice`;
`none` se sem hora válida.

### `POST /api/irrigation/time`  `{ "epoch": 1754500320 }`
Set manual. `struct timeval tv{ .tv_sec = epoch }`;
`perhapsSetRTC(RTCQualityDevice, &tv, /*forceUpdate=*/true)`. Responde `{ok:true, nowEpoch}`.
Validação: epoch plausível (> 1_600_000_000). Audit-log opcional (`config_epoch`-like).

### `POST /api/irrigation/timezone`  `{ "tz": "<-03>3" }`
Valida `tz` contra `TZ_PRESETS` (rejeita fora da lista). Grava
`config.device.tzdef`, `setenv("TZ",tz,1)`, `tzset()`, persiste config
(`nodeDB->saveToDisk(SEGMENT_CONFIG)` — confirmar chamada na fase de impl).
Responde `{ok:true, tz}`.

### `POST /api/irrigation/time/sync`  (sync NTP agora)
Só útil com WiFi STA. Chama helper novo `triggerNtpUpdate()` em `WiFiAPClient`
(zera `lastrun_ntp` file-static → próximo `serialAndWifiHandler` tick refaz o NTP).
Responde `{ok:true, staUp}`. Se `!staUp`, `{ok:false, reason:"sem WiFi"}`.

> **Toque em código compartilhado (aditivo):** expor
> `void triggerNtpUpdate();` em `WiFiAPClient.h/.cpp` que faz `lastrun_ntp = 0;`.
> Guardado por `#ifndef DISABLE_NTP`. Nenhuma mudança de comportamento existente.

## Backend — node-state (relógio no nó)

`buildNodeState` (portal) ganha dois campos:
```json
"nowEpoch": 1754500320,  // getValidTime(RTCQualityDevice,true) do próprio nó
"hasTime": true
```
Nó recebe hora pela malha (mecanismo Meshtastic existente). Display-only.

## Timezone presets

Tabela única de verdade, replicada em C++ (`TZ_PRESETS[]`) e JS:

| label | POSIX |
|---|---|
| America/Sao_Paulo | `<-03>3` |
| America/Manaus | `<-04>4` |
| America/Rio_Branco | `<-05>5` |
| America/Noronha | `<-02>2` |
| UTC | `GMT0` |

(Sem DST no Brasil atual → offsets fixos.) `tzLabel` no `/time` faz o match reverso;
se `tzdef` não bate nenhum preset → `"Personalizado"` (lista mostra nenhum selecionado).

## Frontend — gateway

### `renderHorario()` (nova sub-tela de "Mais")
Espelha o mockup (linhas 1193–1262), 4 cards:

1. **Relógio do gateway** — "Agora: DD/MM/AAAA HH:MM" de `nowEpoch` formatado no
   fuso local do navegador **ou** via `tz`? → usar `nowEpoch` + `Intl`/manual;
   mostrar também `tzLabel`. Se `!hasRtc` → "Sem relógio".
2. **Fonte de hora** — dois botões `NTP (automática)` / `Manual`. Estado inicial =
   `source`. NTP: card com servidor + "última sincronização" (`lastSyncS`) +
   botão **Sincronizar agora** (só se `staUp`). Manual: inputs `date`+`time` +
   botão "Definir data e hora" → `POST /time`.
3. **Fuso horário** — lista de `TZ_PRESETS`; clique → `POST /timezone`; realça o atual.
4. **Sincronização por dispositivo** — reusa `/stations` (e repetidores, se
   expostos) → pill epoch (sincronizada/pendente/inalcançável) por nó.

Registro: adicionar `['horario','Horário','Fonte de hora, fuso e sincronização dos nós']`
em `renderMais()`, `horario: renderHorario` em `RENDER`, `horario:'Horário'` em
`SECTION_LABELS`.

### Header time badge
Novo `<span id="timeBadge">` no `<header>` de `index.html`, ao lado de `#syncChip`.
`renderOverview()` (já roda `/overview`; buscar `/time` junto ou reusar `nowEpoch`
de um fetch leve) atualiza o texto (HH:MM) e cor; `click` → `showSub('horario')`.
Poll já existente (3 s) mantém atualizado.

## Frontend — portal do nó

`renderNode()`: adicionar uma linha/stat **Relógio** no card "Este nó" (estação e
repetidor), formatando `s.nowEpoch` quando `s.hasTime`; senão "sem relógio".
Sem novos controles.

## Simplificação "Fonte de hora" (decisão 6)

Não suprimimos NTP quando o usuário escolhe "Manual" (evita mexer no fluxo
compartilhado do `WiFiAPClient`). Semântica:
- **Sem WiFi** → NTP nunca roda → o set manual persiste (caso de uso real: gateway
  sem internet).
- **Com WiFi** → NTP corrige o relógio (desejável); "Manual" apenas revela os
  campos de ajuste imediato.
`source` no `/time` sempre reflete a realidade (`getRTCQuality`), então a UI não mente.

## Tratamento de erro

- Endpoints só respondem em GATEWAY (`gwIsGateway()`); senão 404/`{ok:false}` como os demais.
- `POST /time` com epoch implausível → `{ok:false, reason:"epoch inválido"}`.
- `POST /timezone` fora do preset → `{ok:false, reason:"fuso desconhecido"}`.
- `POST /time/sync` sem WiFi → `{ok:false, reason:"sem WiFi"}` (UI mostra aviso).
- Frontend: erros exibidos no card (padrão `msg`/`.err` já usado no app.js).

## Testes

- **Nativo (`./bin/run-tests.sh`, via Docker):** testes de builder puro —
  `buildTimeStatus` (source ntp/manual/none, match de tzLabel, lastSyncS=-1),
  validação de preset de fuso, parse de `POST /time` (epoch válido/ inválido).
  Alvo: manter suíte verde, +N casos.
- **Manual/hardware (MCP harness):** definir hora manual sem WiFi; conectar WiFi e
  ver NTP sobrepor; trocar fuso e conferir "Agora"; abrir portal de estação e ver relógio.

## Fora de escopo (YAGNI)

- Config de hora nos nós (só display).
- Servidor NTP customizável pela UI (usa `config.network.ntp_server`).
- Fusos com DST / lista IANA completa.
- Sincronização forçada push de hora para todos os nós (a malha já distribui).

## Riscos / a confirmar na implementação

- Chamada exata de persistência de `config` (`nodeDB->saveToDisk(...)` ou
  `service->reloadConfig`) para `tzdef`.
- Se `getRTCQuality()` está acessível do módulo (`#include "gps/RTC.h"`).
- Exposição de repetidores no `/stations` (o card de sync pode listar só estações
  na 1ª versão se repetidores não estiverem no view).
