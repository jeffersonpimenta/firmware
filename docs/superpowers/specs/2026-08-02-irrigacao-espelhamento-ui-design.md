# Design — Modo Espelhamento UI (tela em "Mais")

Data: 2026-08-02 · Branch: `sistema-irrigacao`

## Objetivo

Dar ao operador uma forma de **ativar/desativar** o modo espelhamento e de **ver o
estado** das entradas espelhadas no painel do gateway. Hoje o `MirrorMode` está
totalmente implementado no firmware mas **nada chama `setEnabled`** — é inalcançável em
produção. Reproduzir a tela "Modo Espelhamento" do mockup (`Irrigacao Mobile.dc.html`,
aba Mais): toggle global + banner de bypass + estado vivo das 4 portas + associações
porta→zona (CRUD, com polaridade e habilitar/pausar por associação).

## O que o espelho faz (recap)

Gateway lê as próprias entradas digitais (`settings.pinsDigitalIn[i]` com polaridade
`settings.digitalInActiveLow` bit i), e quando `mirror.enabled()`, cada entrada ativa
comanda a **abertura da zona associada** (`Zone.fonteInput == i`) no nó dono
(`gwSendValveCmd`, OPEN_S=120 s renovado a 60 s), **bypassando** cronograma, grupos e
intertravamentos para essas zonas. Fail-safe compilado de 120 s permanece.

## Decisões (fechadas com o utilizador)

- UI = **fiel ao mockup** (tela em "Mais": lista + edição de associação).
- `habilitada` **por-associação** persistente → **novo campo `fonteEnabled` na `Zone`**
  (opção A). Associação pausada continua listada ("Desativada"), preserva porta+polaridade.
- Restore/backup e limpeza de cards da aba Sistema = **outro plano** (feature Y).
- Aditivo de rádio: **sem** mudança de protocolo (VERSION=1) nem da ABI de settings v6
  (180 B). A única mudança de ABI é a **tabela `ZoneTable` gateway-only** (flash local).

## Mapeamento mockup → firmware

| Mockup | Firmware | Ação |
|---|---|---|
| toggle global `mirrorEnabled` | `gateway.mirror.enabled()/setEnabled()` (persistido) | endpoint novo |
| associação `porta→zona` | `Zone.fonteInput` (0–3; -1=nenhuma), `byFonte()` | reusar |
| polaridade Normal/Invertido | `settings.digitalInActiveLow` bit i (settings do gateway) | editar bit + salvar |
| estado vivo da porta | `mirror.inputActive(i)` + leitura `rawBitmap` | expor no GET |
| `habilitada` por associação | **novo** `Zone.fonteEnabled` | adicionar campo |

## Arquitetura

### Firmware — modelo

- `Zone` (`GatewayTables.h`): novo `uint8_t fonteEnabled = 1;`. Ativo só quando
  `fonteInput >= 0`. `ZoneTable::serialize/deserialize`: gravar em `buf[off+28]` (dentro
  do `ZONE_ENTRY=32` existente — offsets 28–31 hoje são padding, **sem crescer o
  registo**), zerar o padding explicitamente na escrita, e **bump do marcador de versão**
  da tabela com migração: dados antigos (sem o campo) → `fonteEnabled = 1` para toda zona
  com `fonteInput >= 0` (associações existentes continuam ativas).
- Gate por-associação: `mirrorOwnsZoneOutput` e o laço de drive do espelho
  (IrrigationModule.cpp ~3054) passam a exigir `z->fonteEnabled` além de `mirror.enabled()`
  e `inputActive`. Associação pausada → o scheduler **não** é suprimido para essa zona
  (comportamento normal volta).

### Firmware — camada web pura (native-tested em `test_irrigation_webapi`)

- `parseMirrorToggle(json) -> {bool enabled}`.
- `parseMirrorMapping(json) -> {int input(0..3), uint8 zoneId, bool invertido, bool habilitado}`
  com validação (input 0..3, zoneId existe, zona não pode já estar noutra porta).
- `buildMirror(mirror, zones, settings, liveInputs) -> json`:
  `{enabled, ports:[{i, nome, active, invertido, zoneId, zoneName, habilitado, driving}]×4,
  mappings:[{input, zoneId, zoneName, estNome, saida, invertido, habilitado, inActive, outOpen}]}`.

### Firmware — endpoints (CI/bancada-only, webserver-guarded, excluídos do nativo)

Em `IrrigationWebEndpoints.cpp`, gated `role==GATEWAY`, espelhando handlers existentes:
- `GET  /api/irrigation/mirror` → `buildMirror(...)`.
- `POST /api/irrigation/mirror` `{enabled}` → `mirror.setEnabled` + `saveGwMirror` + audit.
- `POST /api/irrigation/mirror/mapping` `{input,zoneId,invertido,habilitado}` →
  set `zone.fonteInput`/`fonteEnabled` (limpa `fonteInput` de qualquer outra zona nessa
  porta), aplica bit `digitalInActiveLow` + `saveIrrigationSettings(settings)`,
  `zones.upsert` + `saveGwZones` + audit.
- `POST /api/irrigation/mirror/mapping/delete` `{input}` → `fonteInput=-1` na zona daquela
  porta + save.

### Frontend (`data/irrigacao/`)

- `renderEspelhamento()` em app.js, revelada por "Mais" (entrada + `SECTION_LABELS` +
  `RENDER['espelhamento']`), reproduzindo o markup/tokens do mockup: lista (toggle,
  banner bypass, estado das 4 portas ao vivo via poll 3 s, associações com pill/INV/
  Habilitada-Desativada/toggle) e edição (porta pills livres, zona pills, polaridade
  Normal/Invertido, habilitada toggle, salvar, excluir com confirmação).
- Estado de porta é **read-only** (leitura real de GPIO; a linha do mockup "toque para
  simular" é só do mock — no painel real a porta não é clicável).
- `mock.js`: cobrir os 4 endpoints para preview offline.

## Fora de escopo / follow-ups

- Criar zona a partir da tela de espelho (zonas nascem na aba Zonas; aqui só associa).
- Espelho no nó/estação (o `MirrorMode` implementado é gateway-side).
- Cifra/segurança extra da PSK (herdado).

## Testes

- `test_irrigation_webapi`: +casos para `parseMirrorToggle`, `parseMirrorMapping`
  (válido, input inválido, zona inexistente, porta duplicada), `buildMirror` (enabled,
  4 portas, mapping com invertido/desativada). Bump da contagem de casos.
- Suíte nativa completa GREEN (Docker). Endpoints/frontend = CI/bancada-only.
- **Banca 2+ nós exigida**: drive por rádio (entrada ativa → válvula do nó abre),
  bypass do scheduler, pausa por associação — não são native-testáveis.
