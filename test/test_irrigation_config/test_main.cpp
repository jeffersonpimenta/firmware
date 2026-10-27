#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationSettings.h"
#include <new>
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// Constante legada: magic IRR1 (esquema v1..v8, rejeitado pelo migrate v9).
static constexpr uint32_t MAGIC_IRR1 = 0x49525231u;

// ---- Testes de rejeicao: blobs com magic antigo (IRR1) ou tamanho errado ----

static void test_migrate_rejectsBadInput()
{
    IrrigationSettings s;
    // Blob curto demais
    uint8_t buf[sizeof(IrrigationSettings)];
    memset(buf, 0, sizeof(buf));
    uint32_t newMagic = IrrigationSettings::MAGIC;
    memcpy(buf, &newMagic, 4);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, 5, s)); // n < 6

    // Magic errado
    uint8_t buf2[sizeof(IrrigationSettings)];
    IrrigationSettings def;
    memcpy(buf2, &def, sizeof(def));
    buf2[0] ^= 0xFF;
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf2, sizeof(buf2), s));

    // Tamanho correto mas magic IRR1 (legado)
    uint8_t buf3[sizeof(IrrigationSettings)];
    memcpy(buf3, &def, sizeof(def));
    memcpy(buf3, &MAGIC_IRR1, 4);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf3, sizeof(buf3), s));
}

// Blobs IRR1 de tamanhos antigos (v1=40, v3=52, v4=128, v5=176, v6=180, v7=184, v8=208)
// devem ser rejeitados (magic incorreto e/ou tamanho incorreto).
static void test_legacy_blobs_rejected()
{
    IrrigationSettings out;
    uint8_t buf[sizeof(IrrigationSettings)];
    memset(buf, 0, sizeof(buf));
    // Usa magic IRR1 em todos
    memcpy(buf, &MAGIC_IRR1, 4);

    // v1 (40 B)
    uint16_t ver = 1;
    memcpy(buf + 4, &ver, 2);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, IRRIGATION_SETTINGS_V1_SIZE, out));

    // v3 (52 B)
    ver = 3;
    memcpy(buf + 4, &ver, 2);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, IRRIGATION_SETTINGS_V3_SIZE, out));

    // v4 (128 B)
    ver = 4;
    memcpy(buf + 4, &ver, 2);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, IRRIGATION_SETTINGS_V4_SIZE, out));

    // v5 (176 B)
    ver = 5;
    memcpy(buf + 4, &ver, 2);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, IRRIGATION_SETTINGS_V5_SIZE, out));

    // v6 (180 B)
    ver = 6;
    memcpy(buf + 4, &ver, 2);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, IRRIGATION_SETTINGS_V6_SIZE, out));

    // v7 (184 B)
    ver = 7;
    memcpy(buf + 4, &ver, 2);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, IRRIGATION_SETTINGS_V7_SIZE, out));

    // v8 (208 B) com magic IRR1 — tamanho errado (212 esperado) E magic errado
    ver = 8;
    memcpy(buf + 4, &ver, 2);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, IRRIGATION_SETTINGS_V8_SIZE, out));
}

// Blob com magic IRR2 (novo) mas tamanho errado tambem e rejeitado.
static void test_migrate_wrongSize_rejected()
{
    IrrigationSettings def;
    IrrigationSettings out;
    uint8_t buf[sizeof(IrrigationSettings) + 4];
    memcpy(buf, &def, sizeof(def));
    // Tamanho -1 (211)
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, sizeof(IrrigationSettings) - 1, out));
    // Tamanho +1 (213)
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, sizeof(IrrigationSettings) + 1, out));
}

// ---- Testes de struct/defaults v9 ----

static void test_struct_defaults_v9()
{
    IrrigationSettings s;
    TEST_ASSERT_EQUAL_UINT32(IrrigationSettings::MAGIC, s.magic);
    TEST_ASSERT_EQUAL_UINT32(0x49525232u, s.magic); // IRR2
    TEST_ASSERT_EQUAL_UINT16(9, s.version);
    TEST_ASSERT_EQUAL_UINT8(2, s.numValves);
    TEST_ASSERT_EQUAL_UINT16(10, s.hbMinutes);
    TEST_ASSERT_EQUAL_UINT16(1180, s.vbatMinAbrirCentiV);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinBtn);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinLed);
    TEST_ASSERT_EQUAL_UINT16(1220, s.vbatAvisoCentiV);
    TEST_ASSERT_EQUAL_UINT16(1180, s.vbatCriticaCentiV);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsRemoteLed[0]);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsRemoteLed[1]);
    TEST_ASSERT_EQUAL_UINT32(0, s.btnFallbackNode[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s.btnFallbackKind);
}

static void test_v5_size_and_offsets()
{
    TEST_ASSERT_EQUAL_UINT32(212, sizeof(IrrigationSettings));
    TEST_ASSERT_EQUAL_UINT32(12, sizeof(IrrigationSettings::LocalInterlock));
    TEST_ASSERT_EQUAL_UINT32(128, offsetof(IrrigationSettings, localInterlocks));
    IrrigationSettings s;
    TEST_ASSERT_EQUAL_UINT16(9, s.version);
}

// Offsets v6 preservados.
static void test_v6_defaults_and_size()
{
    IrrigationSettings s;
    TEST_ASSERT_EQUAL_UINT16(9, s.version);
    TEST_ASSERT_EQUAL_UINT16(1220, s.vbatAvisoCentiV);
    TEST_ASSERT_EQUAL_UINT16(1180, s.vbatCriticaCentiV);
    TEST_ASSERT_EQUAL_UINT32(212, sizeof(IrrigationSettings));
    TEST_ASSERT_EQUAL_UINT32(176, offsetof(IrrigationSettings, vbatAvisoCentiV));
    TEST_ASSERT_EQUAL_UINT32(178, offsetof(IrrigationSettings, vbatCriticaCentiV));
}

static void test_settings_v7_size()
{
    TEST_ASSERT_EQUAL_UINT(212, sizeof(IrrigationSettings));
}

// Roundtrip v9: blob com magic IRR2 e tamanho 212 e aceito e campos preservados.
static void test_v9_roundtrip()
{
    IrrigationSettings s;
    s.sensores[1] = {36, 1, 0, 30, 0, 300, 3800, 0, 1000, 1, 0}; // analogico bar
    s.pinsGpo[0] = 27;
    s.latE7 = -221234560;
    s.btnFallbackNode[2] = 0x0A0B0C0D;
    s.remoteFallbackMs = 1500;
    uint8_t raw[sizeof(IrrigationSettings)];
    memcpy(raw, &s, sizeof(s));
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_INT8(36, out.sensores[1].pino);
    TEST_ASSERT_EQUAL_INT16(1000, out.sensores[1].engMax);
    TEST_ASSERT_EQUAL_INT32(-221234560, out.latE7);
    TEST_ASSERT_EQUAL_UINT32(0x0A0B0C0D, out.btnFallbackNode[2]);
    TEST_ASSERT_EQUAL_UINT16(1500, out.remoteFallbackMs);
}

static void test_defaultStruct_bytesDeterministic()
{
    // Constroi duas instancias sobre lixo de memoria diferente: todos os bytes
    // (incluindo padding explicito) devem ser identicos — CRC estavel p/ o gateway.
    alignas(IrrigationSettings) uint8_t rawA[sizeof(IrrigationSettings)];
    alignas(IrrigationSettings) uint8_t rawB[sizeof(IrrigationSettings)];
    memset(rawA, 0xAA, sizeof(rawA));
    memset(rawB, 0x55, sizeof(rawB));
    IrrigationSettings *a = new (rawA) IrrigationSettings();
    IrrigationSettings *b = new (rawB) IrrigationSettings();
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(IrrigationSettings));
    a->~IrrigationSettings();
    b->~IrrigationSettings();
}

// ---- Task 4 (greenfield v9): digitalInMode[] apendado, magic "IRR2", version=9 ----
// Testa via migrateIrrigationSettings (caminho real de (de)serializacao).

static void test_digitalInMode_defaults_disabled()
{
    IrrigationSettings s;
    for (uint8_t i = 0; i < IrrigationSettings::MAX_DIGITAL_IN; i++)
        TEST_ASSERT_EQUAL_UINT8((uint8_t)InputMode::DISABLED, (uint8_t)s.digitalInMode[i]);
}

static void test_digitalInMode_roundtrip_via_migrate()
{
    IrrigationSettings src;
    src.digitalInMode[1] = InputMode::FOLLOW;
    src.digitalInMode[2] = InputMode::EDGE_BOTH;
    src.digitalInActiveLow = 0b0010;
    uint8_t raw[sizeof(IrrigationSettings)];
    memcpy(raw, &src, sizeof(raw));
    IrrigationSettings out;
    // Deve passar pelo caminho v9 (magic IRR2 + size=sizeof)
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)InputMode::FOLLOW, (uint8_t)out.digitalInMode[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)InputMode::EDGE_BOTH, (uint8_t)out.digitalInMode[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)InputMode::DISABLED, (uint8_t)out.digitalInMode[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)InputMode::DISABLED, (uint8_t)out.digitalInMode[3]);
    TEST_ASSERT_EQUAL_UINT8(0b0010, out.digitalInActiveLow);
}

static void test_old_magic_blob_rejected()
{
    // Blob com magic IRR1 (antigo) deve ser rejeitado (greenfield), mesmo com tamanho correto.
    IrrigationSettings src;
    uint8_t raw[sizeof(IrrigationSettings)];
    memcpy(raw, &src, sizeof(raw));
    // Sobrescreve magic com IRR1
    memcpy(raw, &MAGIC_IRR1, 4);
    IrrigationSettings out;
    TEST_ASSERT_FALSE(migrateIrrigationSettings(raw, sizeof(raw), out));
}

static void test_digitalInIsButton_uses_mode()
{
    IrrigationSettings s;
    // Todos DISABLED por default
    TEST_ASSERT_FALSE(digitalInIsButton(s, 0));
    TEST_ASSERT_FALSE(digitalInIsButton(s, 1));
    s.digitalInMode[0] = InputMode::FOLLOW;
    TEST_ASSERT_TRUE(digitalInIsButton(s, 0));
    TEST_ASSERT_FALSE(digitalInIsButton(s, 1));
    s.digitalInMode[1] = InputMode::EDGE_TOGGLE;
    TEST_ASSERT_TRUE(digitalInIsButton(s, 1));
    // Fora do range
    TEST_ASSERT_FALSE(digitalInIsButton(s, IrrigationSettings::MAX_DIGITAL_IN));
}

static void test_digitalInMode_offset_and_size()
{
    TEST_ASSERT_EQUAL_UINT32(208, offsetof(IrrigationSettings, digitalInMode));
    TEST_ASSERT_EQUAL_UINT32(212, sizeof(IrrigationSettings));
}

// digitalInLedSlot permanece funcional (deprecado, removido na Task 7).
static void test_digitalInLedSlot_still_works()
{
    IrrigationSettings s;
    // default digitalInLedIdx = 0xFF -> todos os 2-bit fields = 0x3 = 3 (nenhum)
    TEST_ASSERT_EQUAL_UINT8(3u, digitalInLedSlot(s, 0));
    TEST_ASSERT_EQUAL_UINT8(3u, digitalInLedSlot(s, 1));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_migrate_rejectsBadInput);
    RUN_TEST(test_legacy_blobs_rejected);
    RUN_TEST(test_migrate_wrongSize_rejected);
    RUN_TEST(test_struct_defaults_v9);
    RUN_TEST(test_v5_size_and_offsets);
    RUN_TEST(test_v6_defaults_and_size);
    RUN_TEST(test_settings_v7_size);
    RUN_TEST(test_v9_roundtrip);
    RUN_TEST(test_defaultStruct_bytesDeterministic);
    RUN_TEST(test_digitalInMode_defaults_disabled);
    RUN_TEST(test_digitalInMode_roundtrip_via_migrate);
    RUN_TEST(test_old_magic_blob_rejected);
    RUN_TEST(test_digitalInIsButton_uses_mode);
    RUN_TEST(test_digitalInMode_offset_and_size);
    RUN_TEST(test_digitalInLedSlot_still_works);
    exit(UNITY_END());
}

void loop() {}
