# Irrigação — Fase 7b: Grupos hidráulicos (painel web, validação, roteamento)

**Design doc — 2026-07-24**
Spec de origem: `especificacao-irrigacao-mesh.md` §8.13. Roadmap: `2026-07-11-irrigacao-roadmap.md` (Fase 7). Continua `2026-07-24-irrigacao-fase7a-grupos-hidraulicos-design.md`.

## 1. Contexto e recorte

A Fase 7a entregou o **motor** de grupos hidráulicos: `HydraulicGroupTable` (config + persistência LittleFS), `HydraulicGroupEngine` (máquina guiada por ACK — bomba/válvulas, matriz de falhas §8.13), colados no `gwTick`. Não há interface: grupos só existem se pré-carregados na tabela. A Fase 7b fecha a Fase 7 dando **controle e observabilidade ao operador** e corrigindo lacunas de roteamento descobertas na exploração.

Escopo (roadmap Fase 7, linha 7b + decisões de brainstorming 2026-07-24):

1. **Camada web** — CRUD de grupos **+ status ao vivo**; aba "Grupos" do painel do gateway.
2. **Validação** — zona-membro não pode ser saída de espelho (`fonteInput >= 0`); validação estrutural dos campos.
3. **Roteamento** — grupo **isento do cap global de simultaneidade** (`IL_SIMULTANEIDADE`); **fix**: rotear TODOS os pontos de open/close de zona-de-grupo pelo motor (hoje só o scheduler é group-aware).
4. **Controle manual** — botão abrir/fechar grupo inteiro no painel.

**Sem mudança na ABI de estação.** Grupos são orquestração 100% no gateway (7a). 7b acrescenta apenas camada web e roteamento no `IrrigationModule` — nenhuma mudança de protocolo, settings ou blob de réplica.

## 2. Decisões desta fase

| # | Decisão | Escolha |
|---|---|---|
| Q1 | Conteúdo da aba Grupos | CRUD de config **+ status ao vivo** (estado, bomba, zona corrente) |
| Q2 | Cap global `IL_SIMULTANEIDADE` vs grupo | **Opção A** — zonas de grupo isentas; grupo se autolimita por `maxOpen`; cap governa só zonas soltas |
| Q3 | Backup/export de grupos | **Adiado p/ Fase 8** (junto do backup de config completo com PSK) |
| — | Botão manual de grupo | **Sim** — abrir/fechar grupo inteiro pelo painel |
| — | Manual de zona-de-grupo individual | **Rotear pelo motor** (`setDesired`), não válvula direta nem rejeição |

## 3. Arquitetura / layering

Segue o padrão das Fases 5a/6b: lógica pura native-testável em `IrrigationWebApi`, wiring ESP32 em `IrrigationWebEndpoints`, estáticos em `data/irrigacao/`.

```
data/irrigacao/  (aba Grupos: index.html + app.js + style.css)
   | fetch /api/irrigation/groups[...]
   v
IrrigationWebEndpoints  (#if !MESHTASTIC_EXCLUDE_WEBSERVER)
   | monta *View a partir de HydraulicGroupTable + HydraulicGroupEngine
   | chama build*/parse* puros; aplica ações no IrrigationModule
   v
IrrigationWebApi  (puro, sem Arduino — test_irrigation_webapi)
   build/parse + validação estrutural
```

Como em `StationView`/`GwStationSensors`, o estado de runtime (que a camada pura não enxerga) entra por **struct de view** preenchida no endpoint: a config vem de `HydraulicGroupTable`, o vivo de `HydraulicGroupEngine::stateOf()`/`pumpOn()`.

## 4. Endpoints `/api/irrigation/*`

Todos gated em `IrrigationRole::GATEWAY` (como as demais rotas do painel).

| Rota | Método | Corpo / query | Função |
|---|---|---|---|
| `/groups` | GET | — | Lista de config dos grupos |
| `/groups/status` | GET | — | Status ao vivo por grupo |
| `/groups` | POST | `{id,nome,bombaZoneId,zonas:[…],…}` | Upsert. `id==0` ⇒ servidor aloca próximo id livre (1..8) |
| `/groups/delete` | POST | `{id}` | Remove por id |
| `/groups/command` | POST | `{id, acao:"abrir"\|"fechar", durationS?}` | Controle manual do grupo inteiro |

**Alocação de id no create (`id==0`):** corrige o follow-up conhecido da 6b (create manda `id=0`, backend rejeita). O endpoint procura o primeiro id livre na `HydraulicGroupTable` antes do `upsert`; tabela cheia ⇒ erro.

## 5. Modelo JSON

### Config (GET `/groups`, corpo do POST upsert)
```
{ "id":1, "nome":"Setor Norte", "bombaZoneId":9,
  "zonas":[3,4,5], "minOpen":2, "maxOpen":3, "transicao":0,
  "overlapS":10, "startAfterOpenS":5, "stopBeforeCloseS":8,
  "minRunMin":5, "maxStartsHour":6 }
```
Espelha `HydraulicGroup` campo-a-campo. `transicao`: 0=abrir_antes_de_fechar, 1=fechar_antes_de_abrir.

### Status ao vivo (GET `/groups/status`)
```
{ "id":1, "nome":"Setor Norte", "estado":"rodando",
  "bomba":true, "zonaCorrente":4, "abertas":2 }
```
Mapa `HydraulicGroupEngine::State` → string:

| State | string |
|---|---|
| IDLE | `ocioso` |
| OPENING | `abrindo` |
| START_WAIT | `aguardando_partida` |
| PUMP_WAIT_ACK | `partindo_bomba` |
| RUNNING | `rodando` |
| X_OPEN_WAIT / X_OVERLAP / X_CLOSE_WAIT | `transicao` |
| PUMP_OFF_WAIT | `parando_bomba` |
| DRAIN | `drenando` |
| CLOSE_LAST_WAIT | `fechando` |
| DEFERRED | `adiado` |

`zonaCorrente` = `curZone` do runtime; `abertas` = contagem confirmada (nova pequena getter observável no engine, ex.: `openConfirmedCount(groupId)`, no espírito de `stateOf`/`pumpOn`).

## 6. Validação

Dois níveis, para respeitar a fronteira do módulo puro (que não vê `ZoneTable`):

**Estrutural — `parseGroupUpsert` (puro, native-test):**
- `nome` truncado a 15 chars.
- `zoneCount` de `zonas[]` em 1..8; sem zeros no meio.
- `minOpen >= 1`; `maxOpen == 0` (sem teto) **ou** `minOpen <= maxOpen`.
- Campos de tempo dentro dos tipos (`uint16`). `transicao` ∈ {0,1}.
- Coleta erros em `ParseResult` (padrão existente, até 4 erros).

**Semântica — no `IrrigationModule` pós-parse (precisa de `ZoneTable`):**
- Toda zona-membro existe na `ZoneTable`.
- **Nenhuma zona-membro com `fonteInput >= 0`** (saída de espelho): espelho replica entrada externa cegamente; não pode ser sequenciada por bomba. Rejeita com mensagem.
- `bombaZoneId` (se ≠ 0) existe e **não** está na lista de membros.
- Zona-membro não usada por outro grupo — `HydraulicGroupTable::upsert` já recusa; a checagem semântica antecipa a mensagem amigável.

Erros semânticos voltam ao cliente como JSON `{ok:false, erro:"…"}` (mesmo formato dos POSTs existentes).

## 7. Roteamento / re-route

### 7.1 Isenção do cap global (Opção A)
Zonas de grupo **não contam** no cap de `OpenGate`. Já ocorre de fato no scheduler (`IrrigationModule.cpp:~2012`, `continue` desvia a zona para `setDesired` antes de `openGate.request`). 7b torna isso **explícito e garantido em todos os caminhos** e adiciona teste de regressão: um `IL_SIMULTANEIDADE` com cap=1 não deve impedir um grupo de atingir `minOpen=2` (senão a bomba nunca liga — deadlock).

### 7.2 Fix de roteamento (achado na exploração)
Hoje **só o scheduler** é group-aware. Os caminhos manuais mandam válvula direta, ignorando o motor:
- `handleWebCommand` OPEN/CLOSE (`IrrigationModule.cpp:1755-1772`) — comando manual do painel.
- `portalRunNetCommand` gateway-local (`1829-1842`) — comando via portal quando o próprio nó é o gateway.

Consequência: abrir manualmente uma zona-de-grupo liga a válvula **sem a bomba** e sem a coreografia; fechar briga com a sequência do motor (o motor pode reabrir no próximo tick).

**Correção:** helper único no módulo —
```cpp
// Roteia open/close de UMA zona: grupo ⇒ motor; solta ⇒ caminho atual.
// Retorna true se consumiu (era zona de grupo).
bool routeZoneToGroup(uint8_t zoneId, bool open, uint16_t durationS);
```
Chamado no início de cada ponto de open/close (scheduler, `handleWebCommand`, `portalRunNetCommand`). Se `groups.byZone(zoneId)` → aplica `groupEngine.setDesired()` (respeitando `zoneVerdict().bloqueada` como o scheduler faz) e retorna true; senão retorna false e o chamador segue o caminho `OpenGate`/válvula direta atual. O `continue` já presente no scheduler passa a usar o helper, unificando a política.

### 7.3 Controle manual do grupo (`/groups/command`)
- `abrir`: para cada zona-membro não bloqueada por intertravamento, `setDesired(id, zoneId, true, dur)` — o motor respeita `maxOpen` e liga a bomba na sequência. `durationS` default = maior `maxMin` das zonas do grupo (ou um default compilado se sem `maxMin`), clampado pelo teto de 120 min.
- `fechar`: para cada zona-membro, `setDesired(id, zoneId, false, 0)` — o motor sequencia bomba-off → drain → fecha.
- Auditado com `AuditOrigin::PAINEL` (e `GRUPO_HIDRAULICO` no motor, já existente).

## 8. Frontend — aba "Grupos"

Nova aba após "Programas" (grupos orquestram zonas). `index.html`: `<button data-tab="grupos">Grupos</button>`.

- **Lista**: um card por grupo com nome, badge de estado ao vivo (cor por estado), bomba on/off, membros (chips de zona), contadores. Botões editar / excluir / **abrir** / **fechar**.
- **Form upsert**: nome, seletor de bomba (dropdown de zonas GPO), multiselect de zonas-membro (exclui zonas de espelho na UI — o backend reforça), min/max abertas, transição, tempos.
- **Poll**: `/groups/status` no mesmo intervalo do refresh do overview; atualiza só os badges/contadores sem recriar o form aberto.
- Reusa `style.css` (cards/badges/botões existentes das abas 6b).

## 9. Fora de escopo (explicitado)

- **Backup/export de grupos** → Fase 8 (com backup de config completo + PSK).
- **Portal do nó (5b)** — sem mudança; grupos são gateway-only.
- **Helper de proteção local de pressão** (citado no forward-look da spec 7a) — não está na linha 7b do roadmap; adiado. Proteção por pressão já é expressável como intertravamento por sensor (6b).
- Edição de grupos pelo captive portal — não aplicável.

## 10. Testes (native)

- **`test_irrigation_webapi`** (estende): `buildGroups`, `buildGroupsStatus`, `parseGroupUpsert` (casos válidos + cada regra estrutural: minOpen>maxOpen, zoneCount 0 e >8, transicao inválida), `parseGroupDelete`, `parseGroupCommand`.
- **Validação semântica + roteamento**: suite de integração leve (novo `test_group_routing` ou casos em `test_hydraulic_group_engine`) —
  - zona de espelho rejeitada como membro;
  - `IL_SIMULTANEIDADE` cap=1 não trava grupo com `minOpen=2` (regressão da Opção A);
  - open manual de zona-de-grupo vai para o motor (não emite válvula direta);
  - `id==0` no upsert aloca id livre; tabela cheia ⇒ erro.
- `native-suite-count`: 56 → **57** se criar `test_group_routing`; senão 56 (casos em suites existentes). Decisão final no plano.

## 11. Riscos e notas

- **Fronteira pura vs. ZoneTable**: a validação de espelho/existência de zona **não** cabe no parser puro (não vê `ZoneTable`). Fica no módulo; o teste dessa regra é de integração, não do `IrrigationWebApi`. Mantém `IrrigationWebApi` sem dependência de runtime.
- **Consistência de roteamento**: o helper `routeZoneToGroup` deve ser o **único** ponto de decisão; qualquer caminho de open/close que o ignore reintroduz o bug do open-direto-sem-bomba. O plano deve enumerar todos os call-sites.
- **Duração do manual `abrir`**: sem RTC o gateway opera em `millis`; a duração manual é relativa (timer local da estação), então funciona sem wall-clock — coerente com o requisito RTC-opcional.
- **UI de espelho**: excluir zonas de espelho do multiselect é conveniência; a autoridade é o backend (defesa em profundidade).
