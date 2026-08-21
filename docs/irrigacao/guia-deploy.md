# Guia de deploy — sistema de irrigação mesh

Guia único de **configuração + build + gravação + provisionamento** do sistema de
irrigação sobre este fork do Meshtastic. Cobre qualquer placa ESP32 (não só a
`custom_irrigation`).

Assume ambiente de dev pronto (PlatformIO CLI + driver USB-serial + repo clonado). Se
precisar operar o sistema depois de instalado, veja `manual-operacional.md` e
`funcionamento-sistema-irrigacao.md`.

> **Referência rápida (o que editar p/ mudar o quê)** está no fim do documento.

---

## 1. Visão geral

O sistema tem **3 papéis**, escolhidos no 1º boot (não no build) — o mesmo firmware
serve os três:

| Papel | Função |
|---|---|
| **GATEWAY** | Cérebro. Cronograma semanal, grupos hidráulicos, intertravamentos, painel web, gera a PSK da fazenda, adota config das estações. |
| **ESTAÇÃO** | Nó de campo. Aciona válvulas/relés localmente, lê sensores/entradas, executa comandos do gateway, fail-safe local. |
| **SERVIÇO** | Dispositivo do técnico. Cofre de backups por cliente, re-tune de canal, varredura de cobertura, import/export de config. |

Um nó só entra na malha depois de **provisionado** (papel + canal/PSK). Detalhe de como
o sistema funciona por dentro: `funcionamento-sistema-irrigacao.md`.

---

## 2. Pré-requisitos

- **PlatformIO Core** (`pio`) no PATH.
- **Driver USB-serial** da placa: CP210x (Heltec/TTGO recentes) ou CH340 (clones). Sem o
  driver a porta `COMx` não aparece no Windows.
- Repositório clonado; dependências são baixadas pelo PlatformIO no 1º build.

Sanidade: `pio --version` e, no Windows, confirmar a porta em *Gerenciador de
Dispositivos → Portas (COM & LPT)*.

---

## 3. Escolher a placa (env)

Cada placa é um **env** do PlatformIO, definido em `variants/**/platformio.ini`. O env
ativo por default está em `platformio.ini`:

```ini
[platformio]
default_envs = heltec-v3
```

Envs úteis para irrigação:

| Env | Placa | Nota |
|---|---|---|
| `custom_irrigation` | Heltec WiFi LoRa 32 V2 (seed) | Placa do projeto; sem GPS → GPIO33/36 livres p/ irrigação. Veja `custom-irrigation-board.md`. |
| `heltec-v3` | Heltec V3 (ESP32-S3) | Default do repo. |
| `tbeam` | LILYGO T-Beam | Muitas variações; confira sub-variante. |

Listar todos os envs: `pio project config` (ou olhar as pastas em `variants/`).

Você **não precisa** mudar `default_envs` — pode selecionar o env em cada comando com
`-e <env>` (seção 5). Mude `default_envs` só se quiser que builds sem `-e` usem a placa
de irrigação.

---

## 4. Configurar pinos / criar placa

O mapa elétrico de irrigação vive no **`variant.h`** da placa, no bloco
`IRRIGATION_PIN_*`. Ele é consumido por
`src/modules/irrigation/IrrigationBoardDefaults.h` **apenas no 1º boot** (quando não há
config persistida). Depois de provisionar, o blob salvo tem prioridade e o mapa da placa
é ignorado — para re-aplicar o mapa, faça reset de fábrica.

Seed da `custom_irrigation` (`variants/esp32/custom_irrigation/variant.h`):

| Função | GPIO | Nota |
|---|---|---|
| Válvula 0 abre / fecha | 17 / 23 | H-bridge (latch) |
| Válvula 1 abre / fecha | 22 / 32 | H-bridge (latch) |
| Entrada digital 0 / 1 | 38 / 39 | input-only, **pull-up externo**, ativo-baixo (`IRRIGATION_DIN_ACTIVE_LOW=0x03`) |
| Tamper | 37 | input-only, ativo-baixo |
| GPO 0 (relé/bomba) | 33 | livre por não ter GPS |
| Botão multifunção | 0 | PRG onboard (partilhado c/ botão do sistema) |
| LED de estado | 25 | onboard (partilhado c/ LED do sistema) |
| Sensor 0 (opcional) | 36 | ADC1, comentado por default |

### Re-pinar para o hardware real (ex.: L298N + relé)

1. Editar `variants/esp32/custom_irrigation/variant.h`: trocar os `IRRIGATION_PIN_*`
   pelos pinos reais (válvula0 = IN1/IN2, válvula1 = IN3/IN4 no L298N; relé no GPO).
2. Se a placa base mudar, ajustar `board =` no `variants/esp32/custom_irrigation/platformio.ini`
   e os defines de HARDWARE (LoRa/OLED/ADC) no `variant.h`.
3. Recompilar + gravar (seção 5), depois provisionar (seção 7).

### Criar um env de placa novo

Copie a pasta `variants/esp32/custom_irrigation/` para
`variants/esp32/<minha_placa>/`, e no `platformio.ini` dela ajuste:

```ini
[env:minha_placa]
custom_meshtastic_hw_model = 255          ; 255 = PRIVATE_HW (só p/ manifesto/OTA)
custom_meshtastic_hw_model_slug = PRIVATE_HW
custom_meshtastic_architecture = esp32
board_level = extra
extends = esp32_base
board = <board pio da placa>
build_flags =
  ${esp32_base.build_flags}
  -D <DEFINE_DE_HARDWARE>
  -D CUSTOM_IRRIGATION                     ; ativa o mapa IRRIGATION_* do variant.h
  -I variants/esp32/<minha_placa>
```

`extra_configs` no `platformio.ini` raiz já faz `include` de `variants/*/*/platformio.ini`,
então o env novo aparece automaticamente.

> **`hw_model=255` ≠ identidade em rádio.** É só metadata do manifesto de build/OTA. A
> identidade `HW_VENDOR` na malha vem do `-D` de hardware (ex.: `HELTEC_V2_0`). Para um
> modelo privado real, largue o `-D` de hardware herdado e defina o HW direto no
> `variant.h`.

---

## 5. Build + gravação (flash)

```bash
pio run -e custom_irrigation                 # compila
pio run -e custom_irrigation -t upload       # grava firmware via USB
pio run -e custom_irrigation -t uploadfs     # grava assets web (LittleFS: data/irrigacao/)
```

- **`uploadfs` é obrigatório** para o painel/portal web funcionar — os arquivos estáticos
  do painel (`data/irrigacao/`) e do portal (`data/irrigacao/portal/`) vão na partição
  LittleFS, separada do firmware. Refaça `uploadfs` sempre que mudar o front.
- **Porta serial no Windows.** Os envs deixam `upload_port`/`monitor_port`
  **comentados** de propósito (`variants/esp32/esp32-common.ini`) para não quebrar em
  máquinas diferentes. Se o autodetect errar, force a porta:

  ```bash
  pio run -e custom_irrigation -t upload --upload-port COM5
  ```

- **Monitor serial** (log de boot, 115200 baud):

  ```bash
  pio device monitor -e custom_irrigation --port COM5
  ```

- Atualização depois de instalado, **sem cabo**: OTA BLE (seção 9).

---

## 6. Região LoRa + modem preset

> **É configuração de runtime, NÃO de build.** Não existe `#define` de região no
> firmware — a região (ANZ/EU868/US/BR…) e o modem preset são gravados no
> dispositivo, não compilados.

Um nó Meshtastic **não transmite** enquanto a região não é setada. Formas de setar:

- **App Meshtastic oficial** (BLE/serial) → *LoRa → Region / Modem preset*.
- **CLI Meshtastic**: `meshtastic --set lora.region BR --set lora.modem_preset LONG_FAST`.
- No sistema de irrigação, o papel **SERVIÇO** faz **re-tune** de canal/preset em massa
  (portal do device), e o pareamento propaga o canal da fazenda.

Todos os nós da mesma fazenda precisam de **mesma região + mesmo modem preset + mesmo
canal/PSK** para se ouvirem.

---

## 7. 1º boot + provisionamento

No 1º boot (sem config), o nó sobe o **portal** com um **wizard** que pede o papel:

1. Conectar ao portal do nó (AP/BLE, conforme a placa) → o wizard aparece em tela cheia.
2. Escolher o papel:
   - **GATEWAY** → o nó **gera a PSK da fazenda** e vira o dono da malha.
   - **ESTAÇÃO** → entra em modo de pareamento, aguarda o gateway aprovar.
   - **SERVIÇO** → dispositivo do técnico (cofre/backup/re-tune).
3. **Pareamento gateway ↔ estação:** com o gateway numa janela de pareamento (painel →
   Estações → *Aprovar* o nó novo detectado), a estação recebe a PSK/canal e passa a
   receber comandos.
4. Trocar de papel depois = **reset de fábrica** (força-fecha válvulas no mapa antigo e
   reabre o wizard).

Depois de provisionado, a config (zonas, programas, grupos, intertravamentos) é feita
pelo **painel web do gateway**. Operação diária: `manual-operacional.md`.

---

## 8. RTC externo DS3231 (opcional)

O cronograma semanal precisa de **hora real**. Placas sem RTC (ex.: Heltec V2) rodam o
sistema normalmente em modo **manual / espelhamento**, mas o **scheduler fica ocioso sem
hora**. Para agenda por horário, ligar um **DS3231** ao barramento I2C:

- `custom_irrigation`: SDA=GPIO4, SCL=GPIO15 (partilhado com o OLED). O módulo RTC do
  Meshtastic **auto-deteta** — sem alteração de firmware.
- Sem RTC: mirror/manual continuam 100%; o painel calcula as próximas execuções no
  **relógio do celular** (cliente), então a UI ainda mostra a agenda.

---

## 9. OTA BLE (atualização sem abrir a caixa)

Atualização do firmware **sem cabo, sem internet, sem gastar airtime LoRa**: o portal do
nó (aba **Firmware**) apenas **arma** o modo; a imagem é enviada pelo **app Meshtastic
oficial via BLE**, gravada pelo loader stock.

O gate de segurança **recusa a atualização com qualquer zona/bomba/GPO ativo** e
força-fecha tudo antes de reiniciar no loader (o app de irrigação não roda enquanto está
no loader — fail-safe de 120 min fica parado).

Runbook completo (particionamento, preflight, provisionamento de fábrica):
`ota-ble.md`.

---

## 10. Verificação

**Suíte nativa** (lógica pura, roda no host — não precisa de placa):

```bash
./bin/run-tests.sh        # exit 0 GREEN · 1 RED · 2 AMBER · 3 FILTERED
```

No Windows a suíte nativa roda via Docker — ver a memória do projeto
(`windows-native-test-docker`) para o workaround de `cp`/`.claude/worktrees`.

> **Build ESP32 no CI é exigido antes de merge.** Vários endpoints web
> (`hStations*`, `hOverview`, painel/portal) são **webserver-guarded** e **só compilam no
> ESP32**, não no build nativo. A suíte nativa verde **não** garante que o firmware da
> placa compila — sempre confirmar um build ESP32 (CI ou `pio run -e <env>`) antes de ir
> a campo.

---

## 11. Troubleshooting

| Sintoma | Causa / correção |
|---|---|
| Porta COM não aparece | Driver USB-serial ausente (CP210x/CH340). Instalar e reconectar. |
| `upload` erra a porta | Forçar `--upload-port COMx`; fechar monitores serial abertos. |
| Entradas digitais 38/39/37 flutuam | GPIO34-39 do ESP32 **não têm pull interno**; `INPUT_PULLUP` é no-op. Exigem **pull-up externo**. |
| Placa aparece como "Heltec V2" na malha | Esperado na `custom_irrigation` (`-D HELTEC_V2_0`). `hw_model=255` é só manifesto, não identidade em rádio (seção 4). |
| OLED/antena sem alimentação | `VEXT_ENABLE` (GPIO21, ativo-baixo) precisa estar habilitado — alimenta OLED e boost da antena LoRa. |
| Painel web não carrega | Faltou `pio run -t uploadfs` (assets LittleFS). |
| Nó não transmite | Região LoRa não setada (seção 6). |
| Pinos não batem com o `variant.h` | O mapa da placa só vale no **1º boot**. Reset de fábrica p/ re-aplicar, ou editar pela config. |

---

## 12. Checklist de banca (2+ nós) antes de campo

A suíte nativa **não** exercita rádio. Antes de instalar, validar na bancada com pelo
menos gateway + 1 estação:

- [ ] Build ESP32 do env alvo compila e grava (`pio run -e <env> -t upload`).
- [ ] `uploadfs` feito; painel e portal abrem.
- [ ] Região + modem preset + canal iguais em todos os nós.
- [ ] Provisionamento: gateway gera PSK, estação **pareia e aprova**.
- [ ] Comando manual do painel abre/fecha a válvula/relé da estação.
- [ ] Fail-safe: válvula fecha sozinha no teto de 120 min (ou dur configurada).
- [ ] Reset de fábrica **força-fecha** válvulas no mapa antigo.
- [ ] (Se aplicável) re-tune SERVIÇO, espelhamento 24VAC, grupo hidráulico com bomba,
      site survey (aba Cobertura preenche sem storm de replies).

---

## Referência rápida — o que editar p/ mudar o quê

| Quero mudar… | Ficheiro / ação |
|---|---|
| Placa compilada por default | `platformio.ini` → `default_envs` (ou `-e <env>` no comando) |
| Pinos de válvula/relé/entrada | `variants/esp32/<board>/variant.h` → bloco `IRRIGATION_PIN_*` (só 1º boot) |
| Placa base / defines de hardware | `variants/esp32/<board>/platformio.ini` (`board=`) + `variant.h` (defines HW) |
| Criar placa nova | Copiar `variants/esp32/custom_irrigation/`, ajustar `platformio.ini` + `variant.h` |
| Porta serial de gravação | CLI `--upload-port COMx` (envs deixam comentado de propósito) |
| Assets do painel/portal web | `data/irrigacao/` + `data/irrigacao/portal/` → `pio run -t uploadfs` |
| Região LoRa / modem preset | **Runtime** (app/CLI Meshtastic ou re-tune SERVIÇO), não build |
| Papel do nó (gateway/estação/serviço) | Wizard de 1º boot no portal (trocar = reset de fábrica) |
| Agenda por horário | Ligar RTC DS3231 no I2C (auto-detecta) |
| Atualizar sem cabo | OTA BLE — portal, aba Firmware (`ota-ble.md`) |

---

### Docs relacionados

- `funcionamento-sistema-irrigacao.md` — como o sistema funciona por dentro.
- `manual-operacional.md` — operação diária pelo painel.
- `custom-irrigation-board.md` — detalhe da placa seed e re-pinagem.
- `ota-ble.md` — runbook OTA BLE local.
