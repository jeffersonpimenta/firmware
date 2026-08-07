# Supressão de irrigação por previsão meteorológica (Open-Meteo) — design

**Branch:** `sistema-irrigacao`  ·  **Data:** 2026-08-07
**Fase:** meteorologia (segue a 8b — Horário/relógio já entregue; reusa WiFi STA da 8a)

## Objetivo

Suprimir automaticamente programas de irrigação de **zonas e grupos hidráulicos**
quando a previsão da Open-Meteo indica chuva. O gateway consulta a Open-Meteo
**2× por dia**, e no momento em que um programa dispararia, decide por **regras
configuráveis**: se a **chuva prevista acumulada na janela de 12h** e a
**probabilidade de chuva** ultrapassam *ambos* os limiares de uma regra habilitada
que cobre aquele alvo, o disparo é suprimido.

A UI espelha **exatamente** o mockup `Irrigacao Mobile.dc.html` (aba **Mais ›
Meteorologia**). A diferença de arquitetura em relação ao mockup: no mockup o
**navegador** busca a Open-Meteo; aqui quem busca é o **gateway** (decisão autônoma,
vale mesmo sem navegador aberto). A UI fica visualmente idêntica, alimentada por
endpoints do firmware.

## Contexto existente (não reconstruir)

- **WiFi STA sempre-ligado no gateway** (fase 8a) → há conectividade de saída.
- **Relógio/tz** (fase 8b): `getValidTime`, `config.device.tzdef`, hora local via
  `computeLocalSecs` já usada pelo scheduler.
- **Gate do scheduler** vive em `IrrigationModule.cpp:3038–3091`: drena
  `gateway.scheduler.tick()`; no `SchedAction::OPEN` já existem checagens de
  supressão análogas — espelho (`mirrorOwnsZoneOutput`) e **intertravamento**
  (`interlockEngine.zoneVerdict(zoneId).bloqueada` → auditoria + não abre). A
  supressão climática é **mais uma verificação desse tipo**, aplicada antes do
  `routeZoneToGroup`.
- **Tabelas gateway-side** seguem padrão `serialize()/deserialize()` + `MAGIC` +
  persistência em disco (ex.: `ProgramScheduler`, `HydraulicGroupTable`,
  `InterlockTable`, `LevelControlTable`). Config de estação (`IrrigationSettings`)
  é **ABI-locked e do nó** — **não é tocada aqui**.
- **Builders web** puros em `IrrigationWebApi.*`; cola de estado em
  `IrrigationModule.cpp`; rotas em `IrrigationWebEndpoints.cpp`; UI em
  `data/irrigacao/`. JSON é montado à mão (módulo **não usa ArduinoJson** hoje).
- **Auditoria**: `auditEvent(AuditOrigin, AuditAction, zoneId, AuditResult, node)`
  grava no ring existente.
- **`routeZoneToGroup(zoneId, open, dur)`** roteia zonas pertencentes a grupos
  hidráulicos pelo `HydraulicGroupEngine`.

## Decisões (confirmadas com o usuário)

1. **Abordagem A** — subsistema de clima dedicado (config + client de rede + engine
   de veredito puro), separado para manter a decisão testável no suite nativo.
2. Decisão v1 usa **só chuva prevista (12h) + probabilidade**. Chuva passada, ET,
   temperatura ficam fora da lógica (ET/temp/etc. entram só como **exibição** no card).
3. **Tabela de regras**: múltiplas regras, cada uma com limiares próprios, alvos
   próprios e liga/desliga independente. Regra suprime se `chuva > limiarMm` **E**
   `prob > limiarPct` (estritamente maior, como o mockup: `chuva > r.chuvaLimiar &&
   prob > r.probLimiar`).
4. **Alvos = zonas e/ou grupos hidráulicos**, incluíveis/removíveis por regra
   (saídas podem ter função não-irrigação → nunca supressão global).
5. **Localização = manual** (equipamento sem GPS): `latE7`/`lonE7` na config de clima.
6. **Poll 2×/dia** pelo gateway; no fire consulta-se o cache.
7. **Fail-open com TTL**: sem dado fresco (cache mais velho que o TTL, ou nunca
   obtido) → **não suprime** (irriga). Nunca seca por falha de rede.
8. **UI idêntica ao mockup** (Mais › Meteorologia).

## Arquitetura (Abordagem A)

Separação central: **client de rede (impuro)** ↔ **engine de veredito (puro)**. O
veredito é uma função pura testável no suite nativo sem rede.

### Componentes

| Unidade | Arquivo (novo salvo indicado) | Responsabilidade | Interface | Testável nativo |
|---|---|---|---|---|
| `WeatherConfig` | `WeatherConfig.h/.cpp` | Config persistida: enable, lat/lon, horas de poll, TTL | struct + load/save | sim |
| `WeatherRuleTable` | `WeatherRuleTable.h/.cpp` | Tabela de regras (upsert/remove/serialize) | métodos tabela | sim |
| `WeatherCache` | `WeatherForecast.h` | Snapshot do último forecast (métricas + epoch) em RAM | struct POD | sim |
| `WeatherEngine` | `WeatherEngine.h/.cpp` | **Veredito puro**: `verdict(alvo, cache, regras, nowEpoch, ttl)` | funções puras | **sim (núcleo)** |
| `WeatherClient` | `WeatherClient.h/.cpp` | GET Open-Meteo, parse → `WeatherCache`; agendamento 2×/dia | `poll()`, `tick()` | não (rede) — isolado |
| `IrrigationGateway` agregado | `IrrigationGateway.h` (edit) | Passa a conter `weatherConfig`, `weatherRules`, `weatherCache` | membros públicos | — |
| Gate no módulo | `IrrigationModule.cpp` (edit) | Consulta veredito no `OPEN`; pula + auditoria `CLIMA` | glue | — |
| WebApi builders | `IrrigationWebApi.*` (edit) | JSON de status/regras; parse de POST | funções puras | sim |
| Endpoints | `IrrigationWebEndpoints.cpp` (edit) | Rotas HTTP GATEWAY-only | ResourceNode | — |
| UI | `data/irrigacao/*` (edit) | Página Mais › Meteorologia (espelha mockup) | fetch | manual/hw |

## Modelo de dados

### `WeatherRule` (elemento da tabela — ABI de disco gateway-side, versionada)
```
id            : uint8   // 1..255, 0 = slot vazio
enabled       : bool
limiarMmCenti : uint16  // chuva prevista 12h, centi-mm (500 = 5,0 mm)
limiarPct     : uint8   // probabilidade de chuva, %
zonaIds[MAX_ZONE_TARGETS]   : uint8   // 0 = vazio
grupoIds[MAX_GROUP_TARGETS] : uint8   // 0 = vazio
nome[NOME_LEN]     : char  // rótulo, UTF-8, NUL-terminado
mensagem[MSG_LEN]  : char  // texto de alerta exibido na supressão
```
Constantes propostas (ajustáveis na impl): `MAX_WEATHER_RULES = 8`,
`MAX_ZONE_TARGETS = 16`, `MAX_GROUP_TARGETS = 8`, `NOME_LEN = 32`, `MSG_LEN = 48`.
`WeatherRuleTable` segue o padrão `upsert/removeById/count/serialize/deserialize`
das demais tabelas, com `MAGIC` próprio e versão. **Persistido em disco** (arquivo
próprio, como os outros blobs gateway-side).

### `WeatherConfig` (config persistida)
```
enabled     : bool     // master on/off do subsistema
latE7       : int32
lonE7       : int32
pollHourA   : uint8    // hora local do 1º poll (default 4)
pollHourB   : uint8    // hora local do 2º poll (default 16)
staleTtlH   : uint16   // TTL do cache p/ fail-open (default 24 h)
```
`MAGIC` + versão próprios; persistido. Sem alvos aqui (alvos vivem nas regras).

### `WeatherCache` (RAM, não persistido)
Preenchido pelo `WeatherClient` a cada poll bem-sucedido; espelha `parseWeather`
do mockup:
```
valid            : bool
fetchEpoch       : uint32  // getValidTime no momento do fetch (base do TTL)
isMock           : bool    // fetch falhou → estimativa/última? (ver Fail-open)
// decisão:
chuvaPrevista12hCenti : uint16  // soma precip horas i+1..i+12, centi-mm
probChuvaPct          : uint8   // max prob horas i..i+12
// exibição (espelha o card do mockup):
chuvaAcum24hCenti, tempAtualCenti, umidadeRelPct, umidadeSoloPct,
tempMinCenti, tempMaxCenti, ventoRajadaCenti, et0Centi
```
Não persistir o cache: no boot, sem cache → fail-open até o primeiro poll (que
ocorre pouco após o boot; ver Agendamento).

## Veredito (engine puro)

```
struct WeatherVerdict { bool suppress; uint8_t ruleId; };  // ruleId 0 = nenhuma

// Para uma zona:
WeatherVerdict zoneVerdict(uint8_t zoneId, const WeatherCache&, const WeatherRuleTable&,
                           uint32_t nowEpoch, uint16_t ttlHours);
// Para um grupo:
WeatherVerdict groupVerdict(uint8_t groupId, ...);
```
Regras de decisão (espelham `evaluateWeather` do mockup, com fail-open):
1. Se `!cache.valid` **ou** `nowEpoch - cache.fetchEpoch > ttlHours*3600` → `suppress=false`
   (fail-open). *(`nowEpoch==0` sem RTC → também fail-open.)*
2. Senão, para cada regra `enabled` que inclui o alvo:
   `meets = (cache.chuvaPrevista12hCenti > rule.limiarMmCenti) && (cache.probChuvaPct > rule.limiarPct)`.
   Primeira regra que casar → `suppress=true, ruleId=rule.id`.
3. Nenhuma casou → `suppress=false`.

Um alvo-grupo é avaliado pelo `groupId`; suas zonas-membro herdam a supressão via
o gate (ver abaixo). Isto reproduz o `byGroup`→`byZone` do mockup.

## Gate no scheduler (`IrrigationModule.cpp`)

No laço que drena `gateway.scheduler.tick()`, no ramo `SchedAction::Type::OPEN`,
**antes** do `routeZoneToGroup` (linha ~3049):

```cpp
if (a.type == SchedAction::Type::OPEN) {
    // Supressão meteorológica (fail-open embutido no engine).
    WeatherVerdict wv = weatherVerdictForZone(a.zoneId); // resolve grupo-dono se houver
    if (wv.suppress) {
        auditEvent(AuditOrigin::CLIMA, AuditAction::CMD_SUPRIMIDO, a.zoneId,
                   AuditResult::OK, z->node);
        continue; // não abre; próximo tick reavalia
    }
}
```
`weatherVerdictForZone` resolve: se a zona pertence a um grupo hidráulico, aplica
`groupVerdict(grupoId)` **e** `zoneVerdict(zoneId)` (OR — qualquer um suprime),
casando o comportamento `byGroup`+`byZone` do mockup. Como o veredito é fail-open e
reavaliado a cada tick, uma regra que deixe de casar volta a permitir a abertura
naturalmente. **CLOSE nunca é suprimido** (não segura válvula aberta).

> `AuditOrigin::CLIMA` e `AuditAction::CMD_SUPRIMIDO` são **novos** (aditivos ao enum
> de auditoria; confirmar nomes/rotulagem na impl).

## WeatherClient (rede + agendamento)

- **Query** (mesma família de parâmetros do mockup, `parseWeather` idêntico):
  ```
  https://api.open-meteo.com/v1/forecast?latitude=<lat>&longitude=<lon>
    &current=temperature_2m,relative_humidity_2m,precipitation,wind_gusts_10m
    &hourly=precipitation,precipitation_probability,soil_moisture_0_to_1cm
    &daily=temperature_2m_max,temperature_2m_min,wind_gusts_10m_max,et0_fao_evapotranspiration
    &past_days=1&forecast_days=2&timezone=auto
  ```
- **Parse** (espelha `parseWeather`): acha `i = hourly.time.indexOf(current.time)`;
  `chuvaPrevista12h = Σ precip[i+1..i+12]`; `probChuva = max(prob[i..i+12])`;
  `chuvaAcum24h = Σ precip[i-23..i]`; demais métricas de `current`/`daily[dayIdx]`.
  Converter para as unidades inteiras do `WeatherCache`.
- **HTTP/TLS:** `HTTPClient` + `WiFiClientSecure` com `setInsecure()` (Open-Meteo é
  HTTPS; TLS na ESP32 pesa mas cabe; sem verificação de cert — endpoint público).
  Timeout curto (~6 s, como o `AbortController` do mockup). *(Risco de impl:
  confirmar footprint TLS e disponibilidade de `WiFiClientSecure` no build ESP32.)*
- **Parser JSON:** avaliar `ArduinoJson` (checar se já é dependência do projeto);
  senão parse mínimo dirigido aos campos acima. *(Decidir na impl.)*
- **Agendamento** (`WeatherClient::tick(nowLocalSecs)`): dispara `poll()` quando cruza
  `pollHourA`/`pollHourB` (uma vez por ocorrência, guardando o último dia/hora
  disparado) **e** um poll único ~30 s após o boot com WiFi STA up (semear o cache).
  Só roda com `config.enabled` e WiFi STA conectado; sem WiFi → não poll (TTL cuida
  do fail-open).
- **Falha de fetch:** `cache.valid` permanece no último valor válido (se houver) até
  o TTL expirar; a UI marca `isMock/erro` como no mockup (faixa de erro no card). Não
  fabricamos supressão a partir de mock — decisão sempre fail-open sem dado fresco.

## Backend — endpoints (todos GATEWAY-only)

`gwIsGateway()` guard; fora do gateway → 404/`{ok:false}` como os demais.

### `GET /api/irrigation/weather`
Status agregado para a página:
```json
{
  "enabled": true,
  "lat": -23.5, "lon": -46.6,
  "updatedEpoch": 1754500320, "isMock": false, "hasError": false, "errorMsg": "",
  "staUp": true,
  "location": "Casa",              // rótulo (ver Localização/rótulo)
  "metrics": {                     // exibição do card (espelha weatherCard.metrics)
    "chuvaPrevista12h": 8.5, "probChuva": 62, "chuvaAcum24h": 2.4,
    "umidadeSolo": 24, "tempMin": 14, "tempMax": 28, "vento": 18, "et0": 3.6,
    "tempAtual": 21, "umidadeRel": 58
  },
  "statusLabel": "Suprimindo: Pomar",   // meteoStatus (derivado dos vereditos atuais)
  "anySuppressed": true,
  "rules": [                       // espelha weatherRules
    { "id":1, "nome":"Chuva forte prevista", "enabled":true,
      "limiarMm":5, "limiarPct":60, "zonaIds":[], "grupoIds":[1],
      "mensagem":"Chuva prevista nas próximas 12h",
      "triggered":true,                       // regra casando agora
      "chuvaAtual":8.5, "probAtual":62 }
  ]
}
```

### `POST /api/irrigation/weather/config`  `{ "enabled":true, "lat":-23.5, "lon":-46.6 }`
Grava `WeatherConfig` (valida lat/lon plausíveis). Persiste. `{ok:true}`.

### `POST /api/irrigation/weather/rule`  (cria ou edita)
Body = regra (id ausente/0 → cria com novo id; senão edita). Validações espelham
`saveWeatherRule`: nome não-vazio, limiares numéricos, ≥1 alvo (zona ou grupo).
Persiste tabela. `{ok:true, id}`. Erros → `{ok:false, errors:[...]}`.

### `POST /api/irrigation/weather/rule/delete`  `{ "id": 2 }`
Remove regra. Persiste. `{ok:true}`.

### `POST /api/irrigation/weather/refresh`
Botão "Atualizar": força um `poll()` imediato (só com WiFi STA). `{ok:true, staUp}`;
sem WiFi → `{ok:false, reason:"sem WiFi"}`.

## Frontend — `data/irrigacao/` (espelha o mockup)

Página **Mais › Meteorologia** com **duas sub-telas** (lista / edição), idêntica a
`Irrigacao Mobile.dc.html` (linhas 702–839):

- **Registro em "Mais":** adicionar item `['meteo','Meteorologia','Supressão por
  previsão de chuva']` no `renderMais()`; `meteo: renderMeteo` no mapa `RENDER`;
  `meteo:'Meteorologia'` em `SECTION_LABELS`. Sub-telas via `showSub()`/estado local.
- **Lista** (`meteoScreen='list'`):
  1. **Card Open-Meteo** — `location` + "Atualizado {updatedLabel}" (+"estimativa
     offline" se `isMock`); faixa de erro se `hasError`; `tempAtual`/`umidadeRel`;
     grid 2col com as 8 métricas de `metrics`. Botão **Atualizar** → `POST
     .../refresh` e refetch do status.
  2. **Faixa de status** — `statusLabel` ("Suprimindo: …" ou "Sem restrição
     meteorológica no momento") com as cores condicionais do mockup
     (`meteoStatusBg/Border/Color`, azul `oklch(0.55 0.14 230)` quando suprimindo).
  3. **Regras** — botão "+ Nova regra"; cada card: nome, badge **"Suprimindo"** se
     `triggered`, toggle de habilitado (`POST .../rule` com enabled invertido),
     `detalhe` = "Chuva prevista (12h) > Xmm E probabilidade > Y%", `alvosLabel`
     ("Zonas: … · Grupos: …" ou "Nenhum alvo selecionado"), "Leitura atual:
     `chuvaAtual`mm · `probAtual`%". Clique no card → edição.
- **Edição** (`meteoScreen='edit'`): cabeçalho ‹ Meteorologia; título "Nova regra
  meteorológica"/"Editar regra"; bloco de erros; inputs **Nome**, **Chuva prevista
  em 12h > (mm)**, **Probabilidade de chuva > (%)**; nota "Suprime quando **ambas**
  as condições…"; chips **Zonas afetadas** e **Grupos hidráulicos afetados** (fonte:
  `/zones` e `/groups` já existentes); input **Mensagem de alerta**; toggle **Regra
  habilitada**; botão **Salvar regra** (`POST .../rule`); em edição, **Excluir
  regra** com confirmação inline (`POST .../rule/delete`).
- **Overview + rótulos de zona/grupo:** o card/rótulo "Suprimido por meteorologia —
  {motivo}" (mockup linhas 195, 417–418, 2586–2590) usa `statusLabel`/veredito por
  alvo já expostos no `/overview` ou `/weather`. Reusar a `mensagem` da regra
  (`reasonRule.mensagem || nome`) como motivo.

**Localização/rótulo `location`:** o mockup usa nome+desc da estação "casa". No
firmware não há esse conceito 1:1; usar rótulo simples (ex.: nome do nó gateway ou
"Gateway") + coordenadas. Detalhe cosmético, alinhar na impl.

## Persistência

Dois blobs gateway-side novos (config + regras), cada um com `MAGIC`+versão e
`serialize/deserialize`, gravados no mesmo mecanismo de disco das outras tabelas
gateway (confirmar helper de save/load na impl — `saveToDisk`/arquivo dedicado,
como `ProgramScheduler`). **Nenhuma mudança na ABI de `IrrigationSettings` (estação).**

## Tratamento de erro

- Endpoints só no gateway; fora → 404/`{ok:false}`.
- `POST config` com lat/lon implausível → `{ok:false, reason:"coordenadas inválidas"}`.
- `POST rule` inválida → `{ok:false, errors:[...]}` (mensagens do `saveWeatherRule`).
- `POST refresh` sem WiFi → `{ok:false, reason:"sem WiFi"}` (UI mostra aviso).
- Fetch Open-Meteo falho/timeout → mantém cache anterior, marca `hasError/isMock`;
  decisão continua fail-open se o cache passou do TTL.
- Sem RTC válido → scheduler já fica idle; veredito também fail-open.

## Testes

- **Nativo (`./bin/run-tests.sh`, via Docker):**
  - `WeatherEngine` puro: fail-open (cache inválido / velho além do TTL / nowEpoch=0);
    suprime só quando `chuva > mm` **E** `prob > pct` (limites estritos, incl. igualdade
    = não suprime); múltiplas regras (primeira que casa vence); regra desabilitada
    ignorada; alvo por zona vs por grupo; alvo não incluído não é suprimido.
  - `WeatherRuleTable`: upsert/remove/count/serialize↔deserialize round-trip;
    rejeição de blob com `MAGIC`/versão/tamanho inválidos.
  - Builders WebApi: JSON de status/regra; parse/validação de `POST rule`/`config`.
  - Alvo: manter suíte verde, +N casos.
- **Manual/hardware (MCP harness):** configurar lat/lon; criar regra cobrindo um
  grupo; forçar "Atualizar"; simular limiares (regra com limiar baixo → badge
  "Suprimindo" e programa não abre, com evento `CLIMA` no log); desabilitar regra →
  volta a irrigar; derrubar WiFi e confirmar fail-open após TTL.

## Fora de escopo (YAGNI v1)

- Chuva passada, ET, temperatura/geada **na decisão** (ET/temp só exibidos no card).
- Múltiplas localizações (uma por gateway).
- Forecast além de 12h; DST/lista IANA (usa `timezone=auto` da Open-Meteo).
- Servidor de clima alternativo configurável pela UI.
- Push de veredito para os nós (decisão vive no gateway; nós só recebem/omitem comando).
- Persistir o `WeatherCache` entre reboots.

## Riscos / a confirmar na implementação

- **TLS na ESP32:** footprint de `WiFiClientSecure setInsecure()` no build do gateway;
  fallback se a heap não comportar (ex.: reduzir buffers, ou avaliar proxy HTTP).
- **Parser JSON:** `ArduinoJson` disponível? Senão, parse mínimo dos campos usados.
- **Nomes de enum de auditoria** (`AuditOrigin::CLIMA`, `AuditAction::CMD_SUPRIMIDO`).
- **Helper exato de persistência** gateway-side (arquivo/segmento) para os 2 blobs.
- **Sizing das constantes** (`MAX_WEATHER_RULES`, alvos, `NOME_LEN`, `MSG_LEN`) vs. RAM.
- **Fonte dos alvos na UI** (`/zones`, `/groups`) e do rótulo `location`.
