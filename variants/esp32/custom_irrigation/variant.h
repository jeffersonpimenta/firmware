// custom_irrigation — placa própria de irrigação.
// SEED elétrico = Heltec WiFi LoRa 32 V2, SEM GPS (GPIO33/36 livres p/ irrigação).
// Para a board real: trocar `board` no platformio.ini, os defines de HARDWARE abaixo
// (LoRa/OLED/ADC conforme a board) e o bloco IRRIGATION_* pelos pinos do L298N/relé.

// ================= HARDWARE (Heltec V2, sem GPS) =================
#define HAS_GPS 0 // sem GPS → GPIO33 e GPIO36 ficam livres para irrigação

#ifndef USE_JTAG // gpio15 é TDO do JTAG
#define I2C_SDA 4
#define I2C_SCL 15
#endif

#define RESET_OLED 16
#define VEXT_ENABLE 21 // active low: alimenta OLED e boost da antena LoRa
#define LED_POWER 25   // LED onboard (partilhado com pinLed da irrigação)
#define BUTTON_PIN 0   // botão PRG onboard (partilhado com pinBtn da irrigação)

#define USE_RF95
#define LORA_DIO0 26
#ifndef USE_JTAG
#define LORA_RESET 14
#endif
#define LORA_DIO1 35
#define LORA_DIO2 34

#define ADC_MULTIPLIER 3.2 // divisor R12=100k / R10=220k
#define BATTERY_PIN 13
#define ADC_CHANNEL ADC_CHANNEL_4 // GPIO13 = ADC2_CH4 (enum adc_channel_t; framework 3.3.9 rejeita o int de ADC2_GPIO13_CHANNEL)
#define BAT_MEASURE_ADC_UNIT 2

// ================= MAPA DE IRRIGAÇÃO (seed Heltec V2) =================
// Consumido por src/modules/irrigation/IrrigationBoardDefaults.h no 1º boot.
#define IRRIGATION_NUM_VALVES        2
#define IRRIGATION_PIN_VALVE0_A      17 // válvula 0 — abre (H-bridge A)
#define IRRIGATION_PIN_VALVE0_B      23 // válvula 0 — fecha (H-bridge B)
#define IRRIGATION_PIN_VALVE1_A      22 // válvula 1 — abre
#define IRRIGATION_PIN_VALVE1_B      32 // válvula 1 — fecha
#define IRRIGATION_PIN_DIN0          38 // entrada digital 0 (input-only, pull-up externo)
#define IRRIGATION_PIN_DIN1          39 // entrada digital 1 (input-only, pull-up externo)
#define IRRIGATION_DIN_ACTIVE_LOW    0x03 // entradas 0 e 1 ativo-baixo (pull-up externo)
#define IRRIGATION_PIN_TAMPER        37 // tamper (input-only)
#define IRRIGATION_TAMPER_ACTIVE_LOW 1
#define IRRIGATION_PIN_GPO0          33 // saída de nível (relé/bomba) — livre por não ter GPS
#define IRRIGATION_PIN_BTN           0  // botão multifunção (PRG onboard)
#define IRRIGATION_PIN_LED           25 // LED de estado (onboard)
// Opcional — sensor analógico 0 no ADC1 (GPIO36, livre sem GPS). Descomentar p/ ativar:
// #define IRRIGATION_PIN_SENSOR0    36
// GPO1 e válvulas 2..7: sem pino seguro livre na Heltec V2 — ativar na board real.

// ================= CONFIG MESHTASTIC BAKED (WROOM mesh-core) =================
// Fatos de fabrica aplicados no 1o boot / factory reset. Admin cortado + sem
// transporte de escrita externo => imutaveis na pratica. Ver spec
// docs/superpowers/specs/2026-11-24-irrigacao-wroom-mesh-core-design.md
#define USERPREFS_CONFIG_LORA_REGION meshtastic_Config_LoRaConfig_RegionCode_ANZ
#define USERPREFS_LORACONFIG_USE_PRESET true
#define USERPREFS_LORACONFIG_MODEM_PRESET meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST
#define USERPREFS_LORACONFIG_TX_POWER 30
#define USERPREFS_CONFIG_LORA_HOP_LIMIT 4
#define USERPREFS_CONFIG_DEVICE_ROLE meshtastic_Config_DeviceConfig_Role_CLIENT
#define USERPREFS_CONFIG_DEVICE_REBROADCAST_MODE meshtastic_Config_DeviceConfig_RebroadcastMode_ALL

// Canal 0 = malha privada. Definir TODOS os sub-campos: fora do userPrefs.jsonc
// o auto-fill de bin/platformio-custom.py nao roda, e USERPREFS_APPLY_CHANNEL(0)
// referencia cada um.
#define USERPREFS_CHANNELS_TO_WRITE 1
#define USERPREFS_CHANNEL_0_NAME "irriga"
#define USERPREFS_CHANNEL_0_PRECISION 0
#define USERPREFS_CHANNEL_0_IS_MUTED false
#define USERPREFS_CHANNEL_0_UPLINK_ENABLED false
#define USERPREFS_CHANNEL_0_DOWNLINK_ENABLED false
#define USERPREFS_CHANNEL_0_PSK                                                                                                   \
    {                                                                                                                            \
        0xa2, 0xec, 0x79, 0x4f, 0xab, 0x3b, 0x6e, 0x73, 0x0a, 0x5c, 0xff, 0xa7, 0xa4, 0xc7, 0xab, 0xed, 0xdd, 0xaf, 0x6e, 0x3e, \
            0x25, 0x71, 0xba, 0x6d, 0xfb, 0xc5, 0x15, 0xd4, 0x5b, 0x1b, 0x93, 0x14                                              \
    }
