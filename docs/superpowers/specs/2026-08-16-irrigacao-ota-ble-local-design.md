# OTA local por contato (BLE-via-app) — Design

**Data:** 2026-08-16
**Branch:** `sistema-irrigacao`
**Spec-mãe:** `docs/superpowers/plans/especificacao-irrigacao-mesh.md` §3.3 (particionamento/rollback), §9 (OTA no roadmap)
**Roadmap:** `docs/superpowers/plans/2026-07-11-irrigacao-roadmap.md`

## 1. Problema

Nós de irrigação em campo só atualizam firmware por USB (contato físico, abrir a caixa).
A spec §9 lista OTA como item de roadmap ("via Wi-Fi **e** via mesh"); §3.3 já reservou o
particionamento. Esta fase entrega **OTA local por contato via BLE**, reusando o loader OTA
stock do Meshtastic — o técnico atualiza o nó ao lado dele, sem abrir a caixa, sem internet,
**sem gastar airtime LoRa**.

**Não** cobre: OTA via mesh LoRa (inviável — imagem ~2-3 MB vs airtime; briga com toda a
disciplina de airtime já implementada); OTA remoto; assinatura de imagem (§3.3 diz "formato
preparado para assinatura" — fica para o futuro); dual-app simétrico (§3.3 idealizado — a
partição real do Meshtastic é assimétrica: 1 app grande + stub OTA de 640 KB).

## 2. Decisões travadas (brainstorming)

| Decisão | Escolha | Motivo |
|---|---|---|
| Transporte | Local por contato, **não** mesh | Airtime; simplicidade |
| Entrada UI | Portal do nó §7.2 (todos os papéis) | Um só código cobre estação/gateway/serviço |
| Mecanismo | **Reusa loader OTA stock do Meshtastic** (`MeshtasticOTA`) | Cabe em 4 MB; reusa código testado; sem reparticionar |
| Canal de transferência | **BLE via app Meshtastic oficial** | 100% offline no campo; loader WiFi exigiria internet |
| Rollback health-check (§3.3) | **ADIADO** | "sem overengineering"; depende de `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + comportamento do loader |
| Assinatura de imagem | ADIADO | Fora do escopo desta fase |

## 3. Realidade do mecanismo (Path A)

O app roda a partir do `app0` (partição única grande) e **não pode reescrever a própria
partição enquanto executa** — por isso o stub existe. Consequências:

- Nosso portal **não carrega os bytes** do `.bin`. Ele apenas **arma** o modo OTA.
- Ao armar, o app reboota no **loader** (`flashApp`/ota_1). O app de irrigação **não roda**
  no loader: LoRa, scheduler e o **timer fail-safe de 120 min morrem**.
- A transferência dos bytes é feita pelo **app Meshtastic oficial** por BLE; o loader grava
  em `app0` e confere SHA-256 contra o `ota_hash` gravado no arming.

Fluxo completo:

```
portal Firmware → [Entrar em modo atualização]
   → gate de segurança (força-fecha + recusa se ciclo ativo)
   → preflight (loader presente + suporta BLE)
   → saveConfig(network, OTA_BLE, hash) + trySwitchToOTA()
   → reboot no loader
        → app Meshtastic oficial --BLE--> .bin → app0 (loader confere hash)
        → reboot no firmware novo
```

Espelha `AdminModule.cpp:365-407` (o mesmo caminho que o app já dispara pelo canal admin),
mas acionável pelo captive portal que o técnico já usa no provisionamento.

## 4. Entregas

### (A) Gate de segurança pré-OTA — valor central, específico de irrigação

Entrar em modo OTA **para o app**. Válvula latching aberta **fica aberta** (o loader não roda
o timer de 120 min). Portanto, armar OTA **deve**, na ordem:

1. **Recusar** se houver ciclo ativo — qualquer válvula aberta (`valves.isOpen`), qualquer GPO
   ligado (`gpos.isOn`) ou grupo/bomba em operação. Retorna erro ao portal
   (ex.: `{"ok":false,"reason":"cycle_active"}` → "Feche as zonas antes de atualizar").
   Evita cortar irrigação no meio **e** perder o fail-safe do app dentro do loader.
2. Se livre, por garantia: `valves.forceCloseAll()` + `gpos.allOff()` antes do reboot
   (defense-in-depth; o gate 1 já garante que nada está ativo).
3. Auditar: novo `AuditAction::OTA_ARM` (append-only, sem bump de ABI), origem `PAINEL`/portal,
   resultado OK.

Lógica de decisão isolada em **função pura** native-testável (ex.
`bool otaArmAllowed(bool anyValveOpen, bool anyGpoOn, bool groupActive)` em header do módulo),
espelhando o padrão das outras features (predicados puros testados; glue ESP32-only).

### (B) Endpoint de arming (CI-only)

`POST /api/portal/ota` em `IrrigationPortalEndpoints.cpp` (webserver-guarded, **excluído do
nativo** em `variants/native/portduino.ini`). Espelha `AdminModule.cpp:365-407`:

- gate de segurança (A);
- preflight: `MeshtasticOTA::getAppPartition()` / `getAppDesc()` /
  `checkOTACapability(desc, METHOD_OTA_BLE)`; falha → 4xx com motivo
  (`no_loader` / `loader_no_ble`);
- `MeshtasticOTA::saveConfig(&config.network, meshtastic_OTAMode_OTA_BLE, hash)` — modo **BLE**
  (enum `meshtastic_OTAMode_OTA_BLE = 1`); o `ota_hash` vem do corpo do POST (o app que fornece a imagem também
  fornece o hash; se o portal não tiver hash, o arming apenas coloca o loader no ar e o app
  Meshtastic completa o handshake — **verificar na banca** qual dos dois arma de fato o hash);
- `MeshtasticOTA::trySwitchToOTA()` → sucesso: agenda reboot (`rebootAtMsec`, 1-3 s);
  falha: 5xx.

Acessível em **todos os papéis** (portal é all-role).

### (C) Aba "Firmware" no portal

`data/irrigacao/portal/` — nova aba revelada em todos os papéis:

- **Versão atual** do firmware (`APP_VERSION` / `MeshtasticOTA::getVersion()` do app ativo);
- **Status do loader**: presente? suporta BLE? (GET leve — ex. `GET /api/portal/ota/status`);
- botão **"Entrar em modo atualização (BLE)"** → `POST /api/portal/ota`;
- **instruções**: "após confirmar, use o app Meshtastic oficial (BLE) para enviar o arquivo
  `.bin`. O nó ficará ~5-15 min em modo atualização e voltará sozinho."
- desabilita o botão quando há ciclo ativo (feedback do status).

Endpoint/glue/frontend = **CI/banca-only** (webserver-guarded; nunca compilam no nativo).

## 5. Provisionamento (docs + gate de realidade)

Runbook novo em `docs/irrigacao/ota-ble.md`:

1. **Loader OTA** (`mt-esp32-ota.bin`, variante combined **ou** BLE) gravado no `flashApp`/ota_1
   no flash USB de fábrica/bancada. Sem ele → preflight falha; o portal mostra
   "OTA indisponível: loader ausente".
2. **BLE habilitado** no build da variante `custom_irrigation` — confirmar que não foi
   desabilitado (Heltec V2 tem BLE).
3. Procedimento de campo passo a passo (armar → app → aguardar → verificar versão).

## 6. ABI / compatibilidade

- **Aditivo total.** Protocolo VERSION=1 intacto; settings ABI **sem bump** (v8 208 B intacto).
- Único delta de ABI: **1 valor append-only** em `AuditAction` (`OTA_ARM`) — nós antigos apenas
  não conhecem o rótulo; sem quebra de layout.
- Sem tipo de wire novo (OTA é local/BLE, não passa pelo protocolo LoRa da irrigação).

## 7. Testes / verificação

- **Nativo puro:** função de decisão do gate (`otaArmAllowed` — recusa com válvula/GPO/grupo
  ativo; permite quando livre). Suite nova ou casos em suite existente do módulo.
- **Build:** nativo compila+linka `IrrigationModule.cpp`; **build ESP32 no CI exigido**
  (endpoint webserver-guarded + `MeshtasticOTA` são ESP32-only, não compilam no nativo).
- **Banca 1 nó EXIGIDA:**
  1. armar com zona aberta → **recusado** (gate);
  2. armar livre → reboota no loader; app Meshtastic envia `.bin` por BLE → volta no fw novo;
  3. confirmar que válvulas ficaram fechadas durante o modo OTA;
  4. verificar qual lado (portal vs app) arma o `ota_hash` de fato — ajustar (B) conforme.

## 8. Não-escopo / follow-ups

- Rollback health-check §3.3 (imagem pendente confirmada por boot saudável) — **adiado** (fase
  própria; depende de config de bootloader + comportamento do loader).
- Assinatura de imagem — adiado.
- OTA via mesh LoRa — descartado (airtime).
- OTA WiFi (loader baixando por hash) — descartado (exige internet no campo).
- Update só de `littlefs` (UI desacoplada, §3.3) — possível follow-up barato e sem risco.

## 9. Riscos

| Risco | Mitigação |
|---|---|
| Válvula latching aberta perde fail-safe no loader | Gate (A) recusa arming com ciclo ativo + força-fecha |
| Loader ausente/errado no nó | Preflight bloqueia + portal informa; runbook de provisionamento |
| Quem arma o `ota_hash` (portal vs app) | Verificar na banca antes de fixar (B) |
| BLE desabilitado no build | Item de provisionamento; confirmar na variante |
| Imagem ruim sem rollback (C adiado) | Aceito nesta fase; USB recovery sempre disponível; C é follow-up |
