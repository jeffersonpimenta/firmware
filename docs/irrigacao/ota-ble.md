# OTA local por contato (BLE) — runbook

Atualização de firmware do nó **sem abrir a caixa**, com o técnico ao lado, **sem
internet e sem gastar airtime LoRa**. Reusa o loader OTA stock do Meshtastic; o portal
do nó apenas **arma** o modo, e a transferência da imagem é feita pelo **app Meshtastic
oficial por BLE**.

> Referências de projeto: spec-mãe §3.3 (particionamento) e §9 (roadmap OTA);
> design `docs/superpowers/specs/2026-08-16-irrigacao-ota-ble-local-design.md`.

## Como funciona (resumo)

O app roda a partir da partição única grande (`app0`) e **não pode reescrever a própria
partição enquanto executa**. Por isso a atualização passa pelo **loader** (partição
`flashApp`/ota_1):

```
portal do nó → aba Firmware → [Entrar em modo atualização (BLE)]
   → gate de segurança (recusa se houver ciclo ativo; força-fecha válvulas/GPO)
   → preflight (loader presente e com suporte BLE)
   → arma OTA/BLE + reinicia no loader
        → app Meshtastic oficial --BLE--> envia o .bin → grava em app0 (loader confere hash)
        → reinicia no firmware novo
```

Enquanto o nó está no loader, **o app de irrigação não roda**: LoRa, cronograma e o
timer fail-safe de 120 min ficam parados. Por isso o gate força-fecha tudo e recusa a
atualização com qualquer zona/bomba/GPO ativo.

## Provisionamento (uma vez, no flash USB de fábrica/bancada)

1. **Gravar o loader OTA.** Ao flashar o firmware por USB, gravar também a imagem do
   loader `mt-esp32-ota.bin` (variante *combined* ou *BLE*) na partição `ota_1`
   (`flashApp`). O script `bin/device-install.sh` já lê o offset de `ota_1` do
   particionamento e grava `mt-<MCU>-ota.bin` — usar esse fluxo.
   - **Sem o loader**, a aba Firmware mostra *"OTA indisponível: loader ausente"* e o
     botão fica desabilitado.
2. **BLE habilitado.** Confirmar que o build da variante `custom_irrigation` não desabilita
   BLE (a Heltec V2 tem BLE). O loader precisa de BLE para receber a imagem.
   - Se o loader não suportar BLE, a aba mostra *"OTA indisponível: loader sem suporte BLE"*.

## Procedimento de campo

1. **Feche todas as zonas.** O portal recusa a atualização com ciclo ativo (mostra
   *"Feche as zonas ativas antes de atualizar"* e mantém o botão desabilitado).
2. Conecte ao **captive portal** do nó (Wi-Fi do nó) → aba **Firmware**. Confira a
   **versão atual** e a linha de status (deve indicar *"Pronto para atualizar via BLE"*).
3. Toque em **Entrar em modo atualização (BLE)** e confirme. O nó reinicia no loader
   (fica alguns minutos fora do ar).
4. Abra o app **Meshtastic oficial** (Android/iOS) no celular → conecte por **Bluetooth**
   a este nó → função de **atualização de firmware** → selecione o arquivo `.bin`
   (baixado antes, num lugar com internet). O app envia a imagem por BLE (~5–15 min).
5. Aguarde. O nó volta sozinho no firmware novo. Confirme a **nova versão** na aba
   Firmware.

## Segurança e recuperação

- **Gate de ciclo ativo:** entrar em OTA para o app (LoRa/cronograma/fail-safe). Por
  isso o portal recusa a atualização com válvula/bomba/GPO ativo e força-fecha tudo
  antes de reiniciar. Ainda assim, atualize com o sistema ocioso.
- **Auditoria:** o arming registra um evento `OTA_ARM` (origem: painel/portal) no log.
- **Rollback automático (health-check §3.3): NÃO implementado nesta fase** (follow-up).
  A recuperação de uma imagem ruim é sempre possível por **reflash USB**.
- **Assinatura de imagem:** fora do escopo desta fase (o particionamento já está preparado
  para assinatura no futuro, §3.3).

## Verificação em banca (obrigatória antes de campo)

O caminho de rádio/BLE **não é testável no nativo**. Validar com 1 nó:

1. Armar com **uma zona aberta** → deve ser **recusado** (gate).
2. Armar com o sistema **ocioso** → reinicia no loader; app Meshtastic envia o `.bin` por
   BLE → volta no firmware novo; conferir a versão.
3. Confirmar que as **válvulas ficaram fechadas** durante todo o modo de atualização.
4. Verificar **quem arma o `ota_hash`** (portal vs app). Se o app Meshtastic re-arma com o
   próprio hash antes de enviar a imagem, o `hash` opcional do `POST /api/portal/ota` é
   redundante — ajustar o handler se necessário.
