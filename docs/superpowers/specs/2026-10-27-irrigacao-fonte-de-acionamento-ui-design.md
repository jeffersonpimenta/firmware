# Fonte de Acionamento — UI (companion)

> Data: 2026-10-27 · Branch: `sistema-irrigacao`
> **Depende de:** [`2026-10-27-irrigacao-fonte-de-acionamento-unificada-design.md`](./2026-10-27-irrigacao-fonte-de-acionamento-unificada-design.md)
> (núcleo firmware — fonte da verdade do contrato). Firmware mergeia primeiro.

## Objetivo

Funde as duas telas de hoje — **Espelhamento** (`/mirror`, grid de 4 portas
locais do gateway) e **Modo Remoto** (`/remoto`, lista de associações
gateway-cêntrica) — numa única tela **Fonte de Acionamento**, com seletor de
`InputMode` por entrada e sem config de LED dedicado.

Frontend afetado: `data/irrigacao/app.js` (telas `espelhamento` + `remoto` →
`acionamento`). Backend: `IrrigationWebApi` / `ServicePortalApi`.

## Decisões (fechadas no brainstorm)

### 1. Layout: grid por entrada, agrupado por nó, servido no gateway

Uma tela só, modelo **híbrido "B como grid"**:

```
FONTE DE ACIONAMENTO
── Gateway (local) ───────────────────────────
 Entrada 0  ● ativo        [INV]
  Modo:  [ Segue nível          ▾ ]
  Alvo:  [ Zona 2 — Canteiro     ▾ ]   → acionando
 Entrada 1  ○ inativo
  Modo:  [ Borda: abre/fecha     ▾ ]
  Alvo:  [ Zona 3 — Horta        ▾ ]
── Estação "Bomba" 0x7a2f ────────────────────
 Entrada 0  ● ativo
  Modo:  [ Segue nível          ▾ ]
  Alvo:  [ Zona 5 — Reservatório ▾ ]
 Entrada 1  [ Desabilitada       ▾ ]
```

- Seção por nó: entradas do próprio gateway + uma seção por estação conhecida
  (o gateway já recebe heartbeats/telemetria das estações → agrega).
- Cada linha de entrada: estado live (dot), badge `INV`, seletor de modo,
  seletor de zona-alvo, indicador "acionando".

### 2. Modelo de dados: tabela no gateway (modelo B do firmware)

- Binding `(nó,input)→zona` mora na tabela gateway-side (segurança via allowlist
  implícito + view central). `RemoteTrigger = {inputIdx, action}`.
- **Modo/polaridade** (`digitalInMode[i]`, bit de `digitalInActiveLow`) são
  settings do nó dono. Editados no grid do gateway, que **empurra** pro nó via
  `configEpoch` (requisito registrado na spec de firmware).
- Estação mantém editor local mínimo das próprias entradas (fallback sem gateway).

### 3. Seletor de modo: dropdown único plano

Um `<select>` por entrada, com marcação visual:

```
[ Segue nível            ▾ ]
  Desabilitada
  Segue nível          ✓   (atual)
  Borda: abre/fecha
  Borda: toggle        ⚠
  Borda: só abre       ⚠
  Borda: só fecha      ⚠
```

- `⚠` nos três desencorajados (`EDGE_TOGGLE`/`EDGE_OPEN`/`EDGE_CLOSE`) —
  estado ambíguo sem feedback físico. Não bloqueia; só sinaliza.
- Rótulos mapeiam direto no enum `InputMode` do firmware.

### 4. Polaridade: "Normal / Invertido"

Mantém o vocabulário e o controle do Espelho de hoje (botão normal/invertido +
badge `INV`). Sem mudança de termo.

### 5. Feedback: reusa o dot de estado da zona

Sem UI nova de comando. A tela já faz polling do estado **real** da zona (dot
ligado/desligado); o feedback de um acionamento é ver a zona mudar. Não há
badge de "em voo"/"falhou" — isso fica no LED de status de placa (spec de
firmware). Nenhum campo novo no JSON de status por conta de feedback.

## Endpoints (proposta — detalhar na implementação)

Funde `/mirror*` + os endpoints de `/remoto`:

- `GET /acionamento` → `{ nodes: [ { node, name, isSelf, inputs: [ { i, mode,
  invertido, active, targetZoneId, zoneName, driving } ] } ] }`.
- `POST /acionamento/entrada` → `{ node, i, mode, invertido, targetZoneId }`
  (upsert; gateway persiste mapeamento + empurra mode/polaridade pro nó).
- `POST /acionamento/entrada/delete` → `{ node, i }`.
- Endpoints antigos `/mirror`, `/mirror/mapping`, `/mirror/mapping/delete` e os
  de associação remota são **removidos** (greenfield).
- `validateRemoteTriggers` estende para validar `action` + `mode`.

Campos removidos do JSON de config: `pinsRemoteLed`, `digitalInLedIdx`.

## Navegação

- Tabs `espelhamento` e `remoto` → **uma** tab `acionamento` ("Acionamento").
- `ROUTES`/labels/`POLLED` em `app.js` atualizados. Auditoria: o rótulo
  `'Espelho'` em `ACOES_LABEL` vira `'Acionamento'` (ou mantém, decisão de
  implementação).

## Testes

- `test_irrigation_webapi`: `GET/POST /acionamento` (shape, upsert, delete,
  validação de `mode`/`action`/`invertido`, zona inexistente).
- Mock `data/irrigacao/mock.js` atualizado pro novo shape, pra UI rodar offline.
- Remoção de cobertura dos endpoints `/mirror*` antigos.

## Dependências de firmware introduzidas por esta UI

Registradas na spec de firmware (seção "contrato"):

1. Push de `digitalInMode`/`digitalInActiveLow` pro nó dono via `configEpoch`.
2. `GET /acionamento` agrega entradas por nó (gateway precisa expor as entradas
   das estações conhecidas, não só as próprias).

## Fora de escopo

- Núcleo firmware, protocolo, config on-disk — na spec de firmware.
- Controle de Nível / Interlock na mesma tela — unificação futura.
