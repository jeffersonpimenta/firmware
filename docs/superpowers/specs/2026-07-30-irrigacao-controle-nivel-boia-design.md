# Irrigação — Controle de nível por boia (enchimento) — Design

> **Status:** design aprovado (brainstorming 2026-07-30). Próximo: plano de implementação (writing-plans).
> **Branch:** `sistema-irrigacao`. **Fase:** feature aditiva pós-Fase 8.

## Problema

Usuário quer que uma **boia** (chave de nível) num nó controle a **partida/parada de um motor** (bomba)
em outro nó ou no GPO do gateway. Caso de uso: **enchimento automático de reservatório** — a bomba liga
quando o nível está baixo e desliga quando enche, ciclicamente.

## Contexto — o que já existe

O sistema já é **orquestrado pelo gateway** e já tem quase todas as peças:

- **Boia = sensor digital.** `IrrigationSettings::SensorSlot` suporta `tipo=0` (digital) com polaridade
  ativo-baixo (`flags` bit0) e debounce. A estação amostra e reporta no heartbeat.
- **Estação → gateway.** `StationTelemetryCache` guarda o último heartbeat por estação, incluindo o bloco
  de sensores (`sensors[]`, `sensorCount`) e o carimbo de tempo `atMs` (millis do último ouvido).
- **Gateway lê sensor de qualquer nó.** `gwTick` (IrrigationModule.cpp ~2630-2644) já monta um array
  `SensorSnapshot{node, sensorIdx, active, valueCenti}` a partir do cache e alimenta o `InterlockEngine`
  1×/tick. O caminho "sensor remoto → decisão no gateway" está vivo.
- **Motor = zona.** A bomba é uma "zona GPO" comum. `routeZoneToGroup(zoneId, open, dur)` é o ponto único
  de abrir/fechar zona; aplica sequência de bomba do grupo hidráulico, veto de intertravamento
  (`zoneVerdict.bloqueada`) e `max_partidas_hora`. Uma zona vive num nó estação **ou** no GPO do gateway
  (Fase 6b) — então "outro nó ou gateway" já sai de graça: o controlador só aponta um `zoneId`.
- **Fail-safe.** Teto de 120 min por abertura (auto-fecha) + renovação estilo `MirrorMode`
  (reabre a 60s de 120s). `AlertCenter` para alertas.

### A lacuna

O `InterlockEngine` só faz ações **negativas**: `BLOQUEAR_ABERTURA` e `FECHAR_E_BLOQUEAR`. Ele **para/veta**,
nunca **liga**. A boia-desliga (nível alto → fecha+bloqueia bomba) já é um intertravamento comum.
A boia-**liga** (nível baixo → parte a bomba) — a ação **positiva, cíclica, com histerese** — é a peça nova.

## Requisitos (travados no brainstorming)

1. **Enchimento automático.** Liga a bomba no nível baixo, desliga no cheio, ciclicamente.
2. **1 boia só** (1 entrada digital). Histerese por **tempo**: tempo mínimo ligado + mínimo desligado,
   para não bater relé no ponto de corte. (2 boias e sensor analógico ficam fora de escopo.)
3. **Alvo = uma zona** (`targetZoneId`). Cobre bomba em nó estação ou GPO do gateway sem código extra.
4. **Fail-safe na perda de sinal.** Se o gateway não recebe a boia por `staleTimeoutS`, **desliga a bomba
   e gera alerta**. O teto de 120 min sempre corta como segunda camada.
5. **Aditivo total.** Sem mudar a ABI de estação (settings v5 / 176 B), sem novo tipo de wire,
   protocolo VERSION continua 1.

## Abordagem escolhida (A)

Nova **tabela de controle de nível no gateway** + **engine puro**, dirigindo o `routeZoneToGroup` existente.
Mantém o controle (positivo) separado da segurança (intertravamento, negativo); core puro 100% testável.

Rejeitadas: **B** (estender `HydraulicGroup` com sensor de gatilho — bump de ABI do grupo + acopla controle
a "tem que ser grupo"); **C** (ação positiva no `InterlockRule` — mistura conceito de trava/segurança com
controle start/stop temporizado, suja o motor de segurança).

## Arquitetura

### Componentes novos (sempre compilados, native-tested)

**`src/modules/irrigation/LevelControlTable.h`** — modelo de dado + tabela. Espelha `InterlockTable`/
`HydraulicGroupTable` (upsert por id, byId, ruleAt, serialize/deserialize com MAGIC, MAX=4).

```cpp
struct LevelRule {
    uint8_t  id = 0;              // 0 = slot vazio
    uint32_t sensorNode = 0;      // estação dona da boia
    uint8_t  sensorIdx = 0;       // 0..3
    bool     ligaQuandoAtivo = true; // polaridade: liga quando sensor ATIVO (=nível baixo)
    uint8_t  targetZoneId = 0;    // bomba/motor (zona de grupo OU GPO avulso)
    uint16_t minOnS = 30;         // tempo mínimo ligado (anti-chatter)
    uint16_t minOffS = 30;        // tempo mínimo desligado
    uint16_t staleTimeoutS = 90;  // sem leitura da boia → desliga + alerta
    char     mensagem[24] = {0};
};

class LevelControlTable {
  public:
    static constexpr size_t MAX = 4;
    static constexpr uint32_t MAGIC = 0x494C564C; // "ILVL"
    bool upsert(const LevelRule &r);
    bool removeById(uint8_t id);
    const LevelRule *byId(uint8_t id) const;
    const LevelRule *ruleAt(size_t index) const;
    size_t count() const;
    size_t serialize(uint8_t *buf, size_t cap) const;
    bool deserialize(const uint8_t *buf, size_t n);
  private:
    LevelRule rules[MAX];
};
```

**`src/modules/irrigation/LevelControlEngine.h/.cpp`** — **puro**, sem hardware. Guarda estado runtime por
regra (RAM, não persiste): latch ligado/desligado + carimbos `lastOnMs`/`lastOffMs` + flag de alerta-stale
emitido. Uma chamada por tick.

```cpp
struct LevelIntent {
    uint8_t  ruleId = 0;
    uint8_t  zoneId = 0;
    enum class Act : uint8_t { NONE, START, RENEW, STOP, STALE_STOP } act = Act::NONE;
    uint16_t durS = 0;   // para START/RENEW
};

class LevelControlEngine {
  public:
    // snaps/nSnaps: leituras correntes (mesmo array que alimenta o InterlockEngine).
    // atMsByNode: função/tabela para obter o atMs do heartbeat da estação dona (frescor).
    // Escreve até MAX intents; devolve quantos. Chamar 1×/tick.
    size_t evaluate(const LevelControlTable &tbl, const SensorSnapshot *snaps, size_t nSnaps,
                    uint32_t nowMs, uint32_t (*ageMsOf)(void *ctx, uint32_t node), void *ctx,
                    LevelIntent *out, size_t maxOut);
  private:
    struct Rt {
        uint8_t id = 0;
        bool on = false;
        uint32_t lastOnMs = 0, lastOffMs = 0, lastRenewMs = 0;
        bool staleAlerted = false;
    };
    Rt rt[LevelControlTable::MAX];
    Rt &rtFor(uint8_t id);
};
```

Constantes compiladas: `RENEW_INTERVAL_MS = 60000`, `OPEN_CEILING_S` = teto de bomba já usado no repo
(`PUMP_CEILING_S`). Nota de implementação: o frescor vem de `StationTelemetry::atMs` — o glue passa um
`ageMsOf` que faz `nowMs - cache.byNode(node)->atMs` (e trata "estação nunca ouvida" como idade infinita).

### Máquina de estado (por regra)

Por tick, para cada regra ocupada, acha o snapshot da `(sensorNode, sensorIdx)`:

- **Sem snapshot OU idade > `staleTimeoutS`·1000:** intent `STALE_STOP` (glue fecha + alerta 1× na borda);
  segura OFF; ao voltar leitura fresca, `staleAlerted=false` e retoma normal.
- Senão `wantOn = (snap.active == ligaQuandoAtivo)`:
  - **OFF && wantOn && (nowMs − lastOffMs ≥ minOffS·1000)** → `START(dur=OPEN_CEILING_S)`; `on=true`;
    `lastOnMs=lastRenewMs=nowMs`.
  - **ON && !wantOn && (nowMs − lastOnMs ≥ minOnS·1000)** → `STOP`; `on=false`; `lastOffMs=nowMs`.
  - **ON persistente && (nowMs − lastRenewMs ≥ RENEW_INTERVAL_MS)** → `RENEW(dur=OPEN_CEILING_S)`;
    `lastRenewMs=nowMs`.
  - Senão → `NONE` (segura; respeita timers).

Só emite borda (START/STOP/STALE_STOP) + RENEW no intervalo → **sem spam de rádio**.

### Glue — `IrrigationModule`

- **Novo membro** `LevelControlEngine levelEngine;` e a `LevelControlTable` dentro de `IrrigationGateway`
  (junto de `interlocks`/`groups`), persistida no mesmo caminho de save das outras tabelas do gateway.
- **`gwTick`:** logo após montar `snaps` (reusa o array já construído p/ o `InterlockEngine`), chamar
  `levelEngine.evaluate(...)`. Para cada intent:
  - `START`/`RENEW` → `routeZoneToGroup(zoneId, true, durS)` (herda veto de intertravamento + sequência de
    bomba + max_partidas_hora).
  - `STOP` → `routeZoneToGroup(zoneId, false, 0)`.
  - `STALE_STOP` → `routeZoneToGroup(zoneId, false, 0)` + `AlertCenter` (alerta "boia muda") na borda +
    `auditEvent(AuditOrigin::NIVEL, AuditAction::FECHAR, ...)`. `AuditOrigin::NIVEL` é um valor **aditivo**
    novo no enum (ring de auditoria só ganha um código; entradas antigas seguem válidas).
- **Ordem:** avaliar o nível **depois** do bloco de intertravamento no `gwTick`, para que o veto de segurança
  já esteja calculado quando o `routeZoneToGroup` for chamado (o `routeZoneToGroup` já rechecka
  `zoneVerdict.bloqueada`).

### Config web + UI

- **Codec puro** em `IrrigationWebApi` (native-tested em `test_irrigation_webapi`):
  `buildLevelControls(tbl, buf, cap)`, `parseLevelUpsert(json, LevelRule&)`, `parseLevelDelete(json, id&)`.
- **Endpoints CI-only** em `IrrigationWebEndpoints.cpp` (guardados `#if !MESHTASTIC_EXCLUDE_WEBSERVER`,
  excluídos do nativo): `GET/POST /api/irrigation/levels`, `POST /api/irrigation/levels/delete`.
  Gated em `IrrigationRole::GATEWAY`.
- **Aba "Nível"** no painel (`data/irrigacao/`), dentro do menu "Mais": CRUD das regras (escolhe nó+idx da
  boia, zona-alvo, polaridade, min-on/off, timeout de stale, mensagem) + status ao vivo (ligado/desligado,
  última razão, flag stale) via poll.
- **Backup §5.5:** incluir o array de regras de nível em `gwBuildBackup` e no scanner `ServiceBackup`
  (mesmo tratamento de `grupos`), aditivo — perfis antigos sem o campo carregam com tabela vazia.

## Segurança (camadas)

1. Boia muda > `staleTimeoutS` → desliga + alerta.
2. Teto de 120 min sempre corta; o RENEW só mantém vivo enquanto quer-ON **e** boia fresca.
3. `routeZoneToGroup` já checa `zoneVerdict.bloqueada` → um intertravamento de segurança
   (ex.: poço seco, proteção contra rodar seca) **ainda veta/fecha a bomba**, independente do controle
   de nível. Controle e segurança compõem.
4. `max_partidas_hora` se a bomba for zona de grupo hidráulico.

## Testes

- **`test/test_level_control/`** (suite nova) — engine puro: liga só após `minOffS`; para só após `minOnS`;
  segura no meio (timer não cumprido); `STALE_STOP` quando idade > timeout + retoma ao voltar fresco;
  `RENEW` emitido no intervalo enquanto ON; polaridade `ligaQuandoAtivo` invertida.
- **`test/test_level_control_table/`** (suite nova) — roundtrip serialize/deserialize; upsert/removeById/byId;
  tabela cheia rejeita; deserialize corrompido → tabela vazia.
- **`test_irrigation_webapi`** — casos novos p/ `buildLevelControls`/`parseLevelUpsert`/`parseLevelDelete`.
- `test/native-suite-count` **+2**. Suíte completa **GREEN via Docker** (Windows: `MSYS_NO_PATHCONV=1` +
  Docker Desktop; ver memória `windows-native-test-docker`).
- **Banca 2 nós EXIGIDA antes de campo** — o caminho de rádio (boia-nó → gateway → bomba-nó), re-tune,
  frescor real e endpoints (CI/banca-only) não são native-testáveis. Validar: boia baixa → bomba liga;
  boia alta → bomba desliga (após min-on); nó da boia desligado → bomba desliga + alerta; sem chatter no
  ponto de corte; teto 120 min corta mesmo com boia travada em "baixo".

## Fora de escopo (YAGNI)

- 2 boias (baixo+alto) e sensor de nível analógico com 2 limiares.
- Modo esvaziar/drenar (só encher).
- Controle de nível puramente local numa estação sem gateway (a feature é gateway-orquestrada).
- Cifra de PSK no backup (§5.5 segue plaintext, herdado).

## Arquivos

**Novos (sempre compilados):**
- `src/modules/irrigation/LevelControlTable.h`
- `src/modules/irrigation/LevelControlEngine.h` / `.cpp`
- `test/test_level_control/test_main.cpp`
- `test/test_level_control_table/test_main.cpp`

**Modificados:**
- `src/modules/irrigation/IrrigationGateway.h` — membro `LevelControlTable levels;`
- `src/modules/irrigation/IrrigationModule.h` / `.cpp` — `levelEngine`; avaliação no `gwTick`;
  execução de intents; persistência; composição do backup.
- `src/modules/irrigation/IrrigationWebApi.h` / `.cpp` — codecs `buildLevelControls`/`parseLevel*`.
- `src/modules/irrigation/IrrigationWebEndpoints.cpp` — endpoints CI-only.
- `src/modules/irrigation/ServiceBackup.*` — array de níveis no §5.5.
- `data/irrigacao/` — aba "Nível" no painel.
- `test/native-suite-count` — +2.
