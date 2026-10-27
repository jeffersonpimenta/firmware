# Fonte de Acionamento — UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fundir as telas de Espelhamento e Modo Remoto numa única tela "Acionamento" — grid de entradas agrupado por nó, servido no gateway, com seletor de `InputMode`, polaridade Normal/Invertido e feedback via o dot de estado da zona.

**Architecture:** Backend (`IrrigationWebApi`) expõe `GET /acionamento` (grid agregado por nó) e `POST /acionamento/entrada`; remove endpoints `/mirror*` e os de associação remota antiga. Frontend (`data/irrigacao/app.js`) ganha a tela `acionamento` e perde `espelhamento`/`remoto`. Binding mora na tabela do gateway (`RemoteButtonTable`); modo/polaridade são empurrados pro nó dono via `configEpoch` (firmware).

**Tech Stack:** C++ (`JsonWriter`/parsers de `IrrigationWebApi`), JS vanilla (`data/irrigacao/app.js`, `mock.js`), Unity (`test_irrigation_webapi`).

## Global Constraints

- **DEPENDE DO PLANO DE FIRMWARE** (`2026-10-27-irrigacao-fonte-de-acionamento-firmware.md`) — mergeia só depois dele verde. Precisa de: `digitalInMode[]`, `RemoteTrigger.action`, gateway expondo entradas das estações (Task 8 do firmware).
- Spec: `docs/superpowers/specs/2026-10-27-irrigacao-fonte-de-acionamento-ui-design.md`.
- JSON via `JsonWriter` (cuidado com `%lld` em nano-libc — usar `JsonWriter::num`, nunca `snprintf %lld`).
- Buffers de handler HTTP fora da stack (padrão já estabelecido no módulo).
- `trunk fmt`; commits em pt-BR; push → `fork`.
- Rótulos de polaridade: "Normal"/"Invertido" (mantém vocabulário atual). Modos desencorajados marcados com `⚠`.

---

### Task 1: Backend — `GET /acionamento` (grid agregado por nó)

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp` (novo builder `buildAcionamento`, substitui `buildMirror` `:1130-1157`)
- Modify: `src/modules/irrigation/IrrigationWebApi.h` (declaração)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (rota `GET /acionamento`; remover `GET /mirror`)
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Produces: JSON
  ```json
  { "nodes": [ { "node": <u32>, "name": "...", "isSelf": true,
      "inputs": [ { "i": 0, "mode": 1, "invertido": false, "active": true,
                    "targetZoneId": 2, "zoneName": "Canteiro", "driving": true } ] } ] }
  ```
  `mode` = valor numérico do enum `InputMode` (0=DISABLED..5=EDGE_CLOSE). Nó local primeiro (`isSelf`), depois estações do cache (Task 8 do firmware).

- [ ] **Step 1: Escrever o teste que falha**

Em `test/test_irrigation_webapi/test_main.cpp`, acrescentar caso que monta zonas + uma entrada local em `FOLLOW`→zona e verifica que `buildAcionamento` emite `nodes[0].isSelf==true`, `inputs[0].mode==1`, `targetZoneId` e `zoneName` corretos. Seguir o estilo dos testes de `buildMirror` existentes.

- [ ] **Step 2: Rodar e confirmar que falha**

Run: `./bin/run-tests.sh test_irrigation_webapi` — Expected: FALHA (`buildAcionamento` não existe).

- [ ] **Step 3: Implementar `buildAcionamento`**

Substituir `buildMirror` por `buildAcionamento(char*buf,size_t cap, uint32_t selfNode, const ZoneTable&, const RemoteButtonTable&, const IrrigationSettings&, const StationInputsCache&, const bool liveActive[4])`. Emitir o shape acima com `JsonWriter`. Para o nó local, iterar `MAX_DIGITAL_IN`: `mode=settings.digitalInMode[i]`, `invertido=(digitalInActiveLow>>i)&1`, `active=liveActive[i]`, resolver zona-alvo via `RemoteButtonTable.findByTrigger(selfNode,i,...)`. Para estações: iterar o cache do firmware Task 8.

- [ ] **Step 4: Rodar e confirmar verde**

Run: `./bin/run-tests.sh test_irrigation_webapi` — Expected: PASS.

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.cpp src/modules/irrigation/IrrigationWebApi.h src/modules/irrigation/IrrigationWebEndpoints.cpp test/test_irrigation_webapi/test_main.cpp
git add -A
git commit -m "feat(irrigation): GET /acionamento (grid agregado por no)"
```

---

### Task 2: Backend — `POST /acionamento/entrada` (upsert) e `/delete`

**Files:**
- Modify: `src/modules/irrigation/IrrigationWebApi.cpp` (parser `parseAcionamentoEntrada`, substitui `parseMirrorMapping` `:1112` e os parsers de associação remota)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (rotas; remover `/mirror/mapping*`)
- Modify: `src/modules/irrigation/IrrigationModule.cpp` (aplicar: grava `RemoteButtonTable` + empurra `digitalInMode`/polaridade pro nó dono)
- Test: `test/test_irrigation_webapi/test_main.cpp`

**Interfaces:**
- Consumes: Task 1 shape; firmware `gwFireRemote`/push de config.
- Produces: `POST /acionamento/entrada { node, i, mode, invertido, targetZoneId }` → upsert; `POST /acionamento/entrada/delete { node, i }`. Validação: `i∈0..3`, `mode∈0..5`, `targetZoneId` existe (ou 0 p/ limpar), `node` conhecido.

- [ ] **Step 1: Escrever o teste que falha**

Teste: parse de `{"node":123,"i":1,"mode":2,"invertido":true,"targetZoneId":5}` → struct com os campos; rejeita `mode:9` e `i:7`.

- [ ] **Step 2: Rodar e confirmar que falha**

Run: `./bin/run-tests.sh test_irrigation_webapi` — Expected: FALHA.

- [ ] **Step 3: Implementar parser + aplicação**

`parseAcionamentoEntrada` via o mesmo `rd.getInt`/`getBool` dos parsers atuais. Em `IrrigationModule.cpp`, a aplicação: se `node==selfNode`, grava `settings.digitalInMode[i]`/`digitalInActiveLow` local + `RemoteButtonTable` upsert; se estação, grava o mapeamento gateway-side e empurra a config de entrada pra estação (firmware Task 8). Estender `validateRemoteTriggers` p/ `mode`.

- [ ] **Step 4: Rodar e confirmar verde**

Run: `./bin/run-tests.sh test_irrigation_webapi` — Expected: PASS.

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/IrrigationWebApi.cpp src/modules/irrigation/IrrigationWebEndpoints.cpp src/modules/irrigation/IrrigationModule.cpp test/test_irrigation_webapi/test_main.cpp
git add -A
git commit -m "feat(irrigation): POST /acionamento/entrada (upsert+delete) com push de config"
```

---

### Task 3: Backend — remover `/mirror*`, `pinsRemoteLed`, `digitalInLedIdx` do portal de serviço

**Files:**
- Modify: `src/modules/irrigation/ServicePortalApi.cpp` (`:185` `pinLed`/`:183` `digitalInActiveLow`; remover `pinsRemoteLed`/`digitalInLedIdx` do get/set — ver `:244-246`)
- Modify: `src/modules/irrigation/IrrigationWebEndpoints.cpp` (remover rotas `/mirror`, `/mirror/mapping`, `/mirror/mapping/delete` e associação remota antiga)
- Test: `test/test_service_portal`/`test_portal_api` conforme cobertura

**Interfaces:**
- Consumes: firmware Task 4 (campos já removidos da struct).

- [ ] **Step 1: Teste**

Atualizar os testes de portal que afirmavam `pinsRemoteLed`/`digitalInLedIdx` no JSON → remover essas asserções; adicionar que `digitalInMode[i]` aparece no dump de config local da estação.

- [ ] **Step 2: Rodar e confirmar que falha**

Run: `./bin/run-tests.sh test_service_portal` — Expected: FALHA (campos ainda referenciados ou faltando `digitalInMode`).

- [ ] **Step 3: Implementar**

Remover as linhas de `pinsRemoteLed`/`digitalInLedIdx` do serializer e do parser do `ServicePortalApi`; expor `digitalInMode[i]` (array) + `digitalInActiveLow`. Remover as rotas `/mirror*` do `IrrigationWebEndpoints.cpp`.

- [ ] **Step 4: Rodar a suíte inteira**

Run: `./bin/run-tests.sh` — Expected: GREEN.

- [ ] **Step 5: Formatar e commitar**

```bash
trunk fmt src/modules/irrigation/ServicePortalApi.cpp src/modules/irrigation/IrrigationWebEndpoints.cpp
git add -A
git commit -m "refactor(irrigation): remove endpoints /mirror e campos de LED dedicado"
```

---

### Task 4: Frontend — tela `acionamento` (render do grid)

**Files:**
- Modify: `data/irrigacao/app.js` (nova função `renderAcionamento` + estado; aproveitar `mrRender`/`renderRemoto` como base e removê-las)
- Modify: `data/irrigacao/mock.js` (mock de `GET /acionamento`)

**Interfaces:**
- Consumes: Task 1 JSON.
- Produces: `renderAcionamento()` desenha seções por nó, cada entrada com dot live, badge `INV`, `<select>` de modo e `<select>` de zona.

- [ ] **Step 1: Mock do endpoint**

Em `mock.js`, adicionar resposta de `/acionamento` com 1 nó local (2 entradas) + 1 estação, no shape da Task 1.

- [ ] **Step 2: Implementar `renderAcionamento`**

Função pura de render (HTML string) + binding de eventos, no estilo de `mrRender` (`app.js:~2340`) e `rmtListHtml` (`:3064`). Seções `<div>` por nó; para cada entrada, `<select data-acmode data-node data-i>` com as 6 opções:
```js
const MODE_OPTS = [
  [0,'Desabilitada',''],
  [1,'Segue nível','✓-current-handled-in-render'],
  [2,'Borda: abre/fecha',''],
  [3,'Borda: toggle','⚠'],
  [4,'Borda: só abre','⚠'],
  [5,'Borda: só fecha','⚠'],
];
```
O `⚠` vai no texto da `<option>`. Botão Normal/Invertido reusa o componente de polaridade do espelho (`data-mrinvertido` → renomear `data-acinvertido`). Dot de estado reusa `mrDotColor`/`rmtDotColor`.

- [ ] **Step 3: Verificar no mock (visual)**

Rodar a UI com `mock.js` (abrir `data/irrigacao` servido localmente) e conferir que o grid aparece com as duas seções, selects populados, badge INV e dots.

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/app.js data/irrigacao/mock.js
git commit -m "feat(ui): tela Acionamento (grid por entrada agrupado por no)"
```

---

### Task 5: Frontend — ações (POST modo/zona/polaridade) + navegação

**Files:**
- Modify: `data/irrigacao/app.js` (handlers `postJson('/acionamento/entrada', ...)`; `ROUTES`/labels/`POLLED` `:4235-4255`; remover rotas `espelhamento`/`remoto`)

**Interfaces:**
- Consumes: Task 2 endpoints; Task 4 render.

- [ ] **Step 1: Ligar os handlers**

`change` no `<select data-acmode>` → `postJson('/acionamento/entrada', {node,i,mode,invertido,targetZoneId})` e re-render. Idem select de zona e toggle de polaridade. Delete quando `mode==0` (Desabilitada) ou botão remover.

- [ ] **Step 2: Navegação**

Em `ROUTES` trocar `espelhamento`/`remoto` por `acionamento: renderAcionamento`; atualizar labels (`:4249`) para "Acionamento"; `POLLED.acionamento = 1`; remover `espelhamento`/`remoto` de `POLLED`. Remover o código morto de `mrRender`/`renderRemoto` e estados `mrData`/`rmtData`.

- [ ] **Step 3: Verificar fluxo no mock**

Trocar modo de uma entrada → confirmar POST no mock e re-render refletindo. Trocar zona. Inverter polaridade.

- [ ] **Step 4: Commit**

```bash
git add data/irrigacao/app.js
git commit -m "feat(ui): acoes e navegacao da tela Acionamento; remove telas Espelho/Remoto"
```

---

### Task 6: Auditoria + texto + limpeza final

**Files:**
- Modify: `data/irrigacao/app.js` (`ACOES_LABEL` `:1135` — `'Espelho'`→`'Acionamento'`; textos "Modo espelho segue operando" `:130`)
- Modify: docs de UI legadas se referenciadas

- [ ] **Step 1: Atualizar rótulos e textos**

Trocar o rótulo de auditoria e a mensagem do card de relógio (`:130`) para refletir "Acionamento". Garantir que nenhum texto mencione LED dedicado.

- [ ] **Step 2: Rodar a suíte inteira + lint JS**

Run: `./bin/run-tests.sh` — Expected: GREEN. Conferir que `app.js` não referencia `mrData`/`rmtData`/`/mirror`.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "chore(ui): rotulos de auditoria e textos para Acionamento"
```

---

## Self-Review (feito pelo autor do plano)

**Cobertura da spec de UI:**
- Grid por entrada agrupado por nó → Task 1,4. ✓
- Modelo B (tabela gateway) + push configEpoch → Task 2 (+ firmware Task 8). ✓
- Dropdown único de modo com ⚠ → Task 4. ✓
- Polaridade Normal/Invertido → Task 4,5. ✓
- Feedback reusa dot de zona → Task 1 (`driving`/`active`), sem UI nova. ✓
- `/mirror*`+`/remoto`→`/acionamento` → Task 1,2,3,5. ✓
- Remove `pinsRemoteLed`/`digitalInLedIdx` → Task 3. ✓

**Placeholders:** Tasks 4/5 referenciam linhas de `app.js` a reusar; o implementador deve ler as funções citadas (`mrRender`, `rmtListHtml`, `ROUTES`) antes de editar.

**Consistência:** `mode` numérico 0..5 = `InputMode` do firmware; endpoints `/acionamento` e `/acionamento/entrada` idênticos entre Task 1,2,4,5.
