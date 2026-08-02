# Design — Aba Sistema: Restaurar + limpeza de cards

Data: 2026-08-02 · Branch: `sistema-irrigacao`

## Objetivo

Na aba **Sistema** do painel do gateway (`renderSistema`, app.js:2214):
1. **Adicionar botão "Restaurar"** — importa um backup §5.5 e reaplica as **tabelas**
   (zonas, programas, intertravamentos, grupos, estações), **mantendo a PSK/canal atuais**
   (opção B, sem reboot). Complementa o "Baixar backup" já existente.
2. **Remover** os dois cards informativos **"Chave da fazenda"** e **"PIN de aplicação"**
   (sem ação, poluem a tela). Sistema fica = Backup + Restaurar.

## Decisões (fechadas com o utilizador)

- Restore = **só tabelas** (B): **não** toca PSK nem canal, **não** reboota. Recupera
  config num gateway já provisionado sem re-adotar a rede.
- Import é **gateway-side novo** — hoje o gateway só **exporta** (`GET /export`); import
  existe apenas no device SERVICO (vault). Aditivo: **sem** mudança de protocolo/ABI de
  settings; reusa as tabelas gateway existentes.
- Remover os cards chave/PIN por completo.

## Arquitetura

### Firmware — camada pura (native-tested)

Reusa o scanner/estrutura do `ServiceBackup` (`parseEnvelope`/`extractLight`) e os parsers
por-item já existentes em `IrrigationWebApi`. Novo helper puro:
- `extractBackupTables(envelope) -> secções {zonasJson, programasJson, intertravamentosJson,
  gruposJson, estacoesJson}` (key-seek tolerante a chaves extra, como `extractLight`).

A aplicação item-a-item reusa parsers existentes (`parseZoneUpsert`, programa,
intertravamento, `parseGroupUpsert`, estação/epoch). PSK e quaisquer campos de rede são
**ignorados**.

### Firmware — glue gateway (CI/bancada-only)

- `gwImportTables(envelope) -> {ok, applied counts, err}`: valida envelope, extrai secções,
  faz **upsert** de cada zona/programa/intertravamento/grupo; para estações, atualiza
  `StationEntry` e marca `desiredEpoch` para **re-push §5.4** (config volta a ser empurrada
  aos nós). Persiste todas as tabelas (`saveGw*`). **Não** escreve PSK/canal, **não**
  reboota. Emite audit `AuditOrigin::PAINEL` / ação de import.

### Firmware — endpoint (CI/bancada-only, webserver-guarded)

- `POST /api/irrigation/import` — streama o corpo (JSON de backup) para staging (FSCom),
  valida, chama `gwImportTables`, responde `{ok, applied:{zonas,programas,...}, err}`.
  Gated `role==GATEWAY`. Espelha o padrão de streaming do import do ServicePortal.

### Frontend (`data/irrigacao/app.js` — `renderSistema`)

- **Remover** os cards "Chave da fazenda" e "PIN de aplicação".
- **Adicionar** card "Restaurar": `<input type=file accept=.json>` + botão que lê o
  ficheiro, `POST /api/irrigation/import` com o corpo, mostra feedback
  (`applied` counts / erro). Aviso claro: "reaplica as tabelas; mantém a chave da rede;
  as estações recebem a config de volta (re-push)."
- `mock.js`: cobrir `POST /import` (echo de counts) para preview offline.

## Fora de escopo / follow-ups

- Restore **completo** com PSK + re-tune de canal (opção A) — adiado; este corte é B.
- Cifra do backup §5.5 (segue plaintext, herdado).
- Import via portal do device SERVICO já existe (vault) — não é tocado aqui.

## Testes

- `test_irrigation_webapi` (ou suíte de backup): +casos para `extractBackupTables`
  (extrai as 5 secções; tolera chaves extra/ordem; envelope inválido falha). Bump da
  contagem de casos.
- Suíte nativa completa GREEN (Docker). Endpoint/glue/frontend = CI/bancada-only.
- **Banca exigida**: import real + re-push de estações por rádio não é native-testável.
