#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationSettings.h"
#include <new>
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// Monta uma imagem v1 (40 B) byte a byte, no layout documentado no header da v1.
static void buildV1Image(uint8_t *img)
{
    memset(img, 0, IRRIGATION_SETTINGS_V1_SIZE);
    uint32_t magic = IrrigationSettings::MAGIC;
    uint16_t version = 1;
    memcpy(img + 0, &magic, 4);
    memcpy(img + 4, &version, 2);
    img[6] = 1;                       // role = GATEWAY
    img[7] = 4;                       // numValves
    uint32_t gw = 0xa1b2c3d4;
    memcpy(img + 8, &gw, 4);          // boundGateway
    uint16_t hb = 15, vbat = 1200, maxOpen = 600;
    memcpy(img + 12, &hb, 2);
    memcpy(img + 14, &vbat, 2);
    memcpy(img + 16, &maxOpen, 2);
    img[18] = 6;                      // cmdRatePerMin  (offset 19 = padding)
    uint16_t pulse = 80;
    memcpy(img + 20, &pulse, 2);
    for (int i = 0; i < 8; i++) {
        img[22 + i] = (uint8_t)(int8_t)(i < 4 ? 10 + i : -1); // pinsHbridgeA
        img[30 + i] = (uint8_t)(int8_t)(i < 4 ? 20 + i : -1); // pinsHbridgeB
    }
}

static void test_migrate_v1_preservesFieldsAndDefaultsNew()
{
    uint8_t img[IRRIGATION_SETTINGS_V1_SIZE];
    buildV1Image(img);
    IrrigationSettings s;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(img, sizeof(img), s));
    TEST_ASSERT_EQUAL_UINT16(6, s.version);
    TEST_ASSERT_EQUAL_UINT8(1, s.role);
    TEST_ASSERT_EQUAL_UINT8(4, s.numValves);
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, s.boundGateway);
    TEST_ASSERT_EQUAL_UINT16(15, s.hbMinutes);
    TEST_ASSERT_EQUAL_UINT16(1200, s.vbatMinAbrirCentiV);
    TEST_ASSERT_EQUAL_UINT16(600, s.maxOpenConfigS);
    TEST_ASSERT_EQUAL_UINT8(6, s.cmdRatePerMin);
    TEST_ASSERT_EQUAL_UINT16(80, s.pulseMs);
    TEST_ASSERT_EQUAL_INT8(12, s.pinsHbridgeA[2]);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsHbridgeA[7]);
    TEST_ASSERT_EQUAL_INT8(22, s.pinsHbridgeB[2]);
    // Campos novos: defaults
    TEST_ASSERT_EQUAL_UINT32(0, s.configEpoch);
    TEST_ASSERT_EQUAL_INT8(-1, s.pinsDigitalIn[0]);
    TEST_ASSERT_EQUAL_UINT8(0, s.digitalInActiveLow);
}

static void test_migrate_v1_getsDefaultButtonLedPins()
{
    uint8_t img[IRRIGATION_SETTINGS_V1_SIZE];
    buildV1Image(img);
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(img, sizeof(img), out));
    TEST_ASSERT_EQUAL_UINT16(6, out.version);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinBtn);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinLed);
}

static void test_migrate_v2_getsDefaultButtonLedPins()
{
    // blob v2 genuíno: 52 bytes com version=2, pinos ainda não existiam
    uint8_t raw[IRRIGATION_SETTINGS_V3_SIZE];
    memset(raw, 0, sizeof(raw));
    IrrigationSettings v2like;
    v2like.pinBtn = 0; // lixo nos bytes que eram pad no v2
    v2like.pinLed = 0;
    memcpy(raw, &v2like, IRRIGATION_SETTINGS_V3_SIZE);
    uint16_t ver2 = 2;
    memcpy(raw + 4, &ver2, 2);
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_UINT16(6, out.version);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinBtn); // v2 não tinha o campo: default
    TEST_ASSERT_EQUAL_INT8(-1, out.pinLed);
}

static void test_migrate_v3_passthroughKeepsPins()
{
    // blob v3 genuíno: 52 bytes com version=3
    uint8_t raw[IRRIGATION_SETTINGS_V3_SIZE];
    memset(raw, 0, sizeof(raw));
    IrrigationSettings v3like;
    v3like.pinBtn = 0;
    v3like.pinLed = 2;
    memcpy(raw, &v3like, IRRIGATION_SETTINGS_V3_SIZE);
    uint16_t ver3 = 3;
    memcpy(raw + 4, &ver3, 2);
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_UINT16(6, out.version);
    TEST_ASSERT_EQUAL_INT8(0, out.pinBtn);
    TEST_ASSERT_EQUAL_INT8(2, out.pinLed);
}

static void test_migrate_v3_passthrough()
{
    // blob v3 genuíno: 52 bytes com version=3
    uint8_t raw[IRRIGATION_SETTINGS_V3_SIZE];
    memset(raw, 0, sizeof(raw));
    IrrigationSettings v3like;
    v3like.configEpoch = 17;
    v3like.pinsDigitalIn[1] = 36;
    v3like.digitalInActiveLow = 0b0010;
    v3like.numValves = 3;
    memcpy(raw, &v3like, IRRIGATION_SETTINGS_V3_SIZE);
    uint16_t ver3 = 3;
    memcpy(raw + 4, &ver3, 2);
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_UINT16(6, out.version);
    TEST_ASSERT_EQUAL_UINT32(17, out.configEpoch);
    TEST_ASSERT_EQUAL_INT8(36, out.pinsDigitalIn[1]);
    TEST_ASSERT_EQUAL_UINT8(0b0010, out.digitalInActiveLow);
    TEST_ASSERT_EQUAL_UINT8(3, out.numValves);
}

static void test_migrate_v2_preservesFieldsResetsPins()
{
    // blob v2 genuíno: 52 bytes com version=2
    uint8_t raw[IRRIGATION_SETTINGS_V3_SIZE];
    memset(raw, 0, sizeof(raw));
    IrrigationSettings v2like;
    v2like.configEpoch = 17;
    v2like.numValves = 5;
    v2like.pinsDigitalIn[2] = 36;
    v2like.digitalInActiveLow = 0b0100;
    v2like.pinBtn = 13; // bytes que eram padding no v2: NÃO podem sobreviver
    v2like.pinLed = 14;
    memcpy(raw, &v2like, IRRIGATION_SETTINGS_V3_SIZE);
    uint16_t ver2 = 2;
    memcpy(raw + 4, &ver2, 2);
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_UINT16(6, out.version);
    TEST_ASSERT_EQUAL_UINT32(17, out.configEpoch);
    TEST_ASSERT_EQUAL_UINT8(5, out.numValves);
    TEST_ASSERT_EQUAL_INT8(36, out.pinsDigitalIn[2]);
    TEST_ASSERT_EQUAL_UINT8(0b0100, out.digitalInActiveLow);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinBtn);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinLed);
}

static void test_migrate_rejectsBadInput()
{
    IrrigationSettings s;
    uint8_t img[IRRIGATION_SETTINGS_V1_SIZE];
    buildV1Image(img);

    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, 10, s)); // curto

    img[0] ^= 0xFF; // magic errado
    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, sizeof(img), s));
    img[0] ^= 0xFF;

    img[4] = 3; // versão 3 válida, mas tamanho 40 != 52 → rejeitado
    TEST_ASSERT_FALSE(migrateIrrigationSettings(img, sizeof(img), s));
}

static void test_migrate_v5_wrongSize_rejected()
{
    // Blob v5 com 175 bytes (um a menos): deve ser rejeitado.
    IrrigationSettings in;
    IrrigationSettings out;
    TEST_ASSERT_FALSE(migrateIrrigationSettings((const uint8_t *)&in, sizeof(in) - 1, out));
}

static void test_migrate_v4_wrongSize_rejected()
{
    // Blob v4 com tamanho incorreto (127 e 129 bytes): deve ser rejeitado.
    uint8_t buf[130];
    uint32_t magic = IrrigationSettings::MAGIC;
    uint16_t ver4 = 4;

    memset(buf, 0, sizeof(buf));
    memcpy(buf + 0, &magic, 4);
    memcpy(buf + 4, &ver4, 2);
    IrrigationSettings out;
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, 127, out)); // v4 exige exatamente 128 B

    memset(buf, 0, sizeof(buf));
    memcpy(buf + 0, &magic, 4);
    memcpy(buf + 4, &ver4, 2);
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, 129, out)); // também inválido
}

static void test_migrate_v2_wrongSize_rejected()
{
    // Blob sintético: magic correto + version=2, mas apenas 51 bytes (v2/v3 exigem exatamente 52).
    uint8_t buf[51];
    memset(buf, 0, sizeof(buf));
    uint32_t magic = IrrigationSettings::MAGIC;
    memcpy(buf + 0, &magic, 4);
    uint16_t ver2 = 2;
    memcpy(buf + 4, &ver2, 2);
    IrrigationSettings out;
    TEST_ASSERT_FALSE(migrateIrrigationSettings(buf, sizeof(buf), out));
}

static void test_migrate_v3_to_v4()
{
    // Blob v3 sintético: struct atual "rebaixada" — monta 52 bytes com version=3.
    IrrigationSettings v4;
    v4.numValves = 3;
    v4.pinBtn = 0;
    v4.configEpoch = 9;
    uint8_t raw[IRRIGATION_SETTINGS_V3_SIZE];
    memcpy(raw, &v4, IRRIGATION_SETTINGS_V3_SIZE); // prefixo v3 == primeiros 52 bytes do v4
    uint16_t ver3 = 3;
    memcpy(raw + 4, &ver3, 2);

    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, IRRIGATION_SETTINGS_V3_SIZE, out));
    TEST_ASSERT_EQUAL_UINT16(6, out.version);
    TEST_ASSERT_EQUAL_UINT8(3, out.numValves);
    TEST_ASSERT_EQUAL_UINT32(9, out.configEpoch);
    // Campos novos em default
    TEST_ASSERT_EQUAL_INT8(-1, out.pinsGpo[0]);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinsGpo[1]);
    TEST_ASSERT_EQUAL_INT8(-1, out.pinTamper);
    TEST_ASSERT_EQUAL_INT32(0, out.latE7);
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL_INT8(-1, out.sensores[i].pino);
}

static void test_v5_roundtrip_and_size()
{
    TEST_ASSERT_EQUAL_size_t(180, sizeof(IrrigationSettings));
    IrrigationSettings s;
    s.sensores[1] = {36, 1, 0, 30, 0, 300, 3800, 0, 1000, 1, 0}; // analógico bar
    s.pinsGpo[0] = 27;
    s.latE7 = -221234560;
    uint8_t raw[sizeof(IrrigationSettings)];
    memcpy(raw, &s, sizeof(s));
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, sizeof(raw), out));
    TEST_ASSERT_EQUAL_INT8(36, out.sensores[1].pino);
    TEST_ASSERT_EQUAL_INT16(1000, out.sensores[1].engMax);
    TEST_ASSERT_EQUAL_INT32(-221234560, out.latE7);
}

static void test_defaultStruct_bytesDeterministic()
{
    // Constrói duas instâncias sobre lixo de memória diferente: todos os bytes
    // (incluindo padding explícito) devem ser idênticos — CRC estável p/ o gateway.
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

static void test_v5_size_and_offsets()
{
    TEST_ASSERT_EQUAL_UINT32(180, sizeof(IrrigationSettings));
    TEST_ASSERT_EQUAL_UINT32(12, sizeof(IrrigationSettings::LocalInterlock));
    TEST_ASSERT_EQUAL_UINT32(128, offsetof(IrrigationSettings, localInterlocks));
    IrrigationSettings s;
    TEST_ASSERT_EQUAL_UINT16(6, s.version);
}

// Fase 9 (v6): defaults, tamanho e offsets dos limiares de bateria.
static void test_v6_defaults_and_size()
{
    IrrigationSettings s;
    TEST_ASSERT_EQUAL_UINT16(6, s.version);
    TEST_ASSERT_EQUAL_UINT16(1220, s.vbatAvisoCentiV);
    TEST_ASSERT_EQUAL_UINT16(1180, s.vbatCriticaCentiV);
    TEST_ASSERT_EQUAL_UINT32(180, sizeof(IrrigationSettings));
    TEST_ASSERT_EQUAL_UINT32(176, offsetof(IrrigationSettings, vbatAvisoCentiV));
    TEST_ASSERT_EQUAL_UINT32(178, offsetof(IrrigationSettings, vbatCriticaCentiV));
}

// Fase 9 (v6): blob v5 genuíno (176 B, version=5) migra p/ v6 com limiares default,
// preservando o prefixo v5 (ex.: latE7).
static void test_migrate_v5_to_v6_fills_defaults()
{
    IrrigationSettings v5;
    v5.latE7 = -221234560;
    uint8_t raw[IRRIGATION_SETTINGS_V5_SIZE];
    memcpy(raw, &v5, IRRIGATION_SETTINGS_V5_SIZE); // primeiros 176 B = layout v5
    uint16_t ver5 = 5;
    memcpy(raw + 4, &ver5, 2);
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, IRRIGATION_SETTINGS_V5_SIZE, out));
    TEST_ASSERT_EQUAL_UINT16(6, out.version);
    TEST_ASSERT_EQUAL_UINT16(1220, out.vbatAvisoCentiV);
    TEST_ASSERT_EQUAL_UINT16(1180, out.vbatCriticaCentiV);
    TEST_ASSERT_EQUAL_INT32(-221234560, out.latE7);
}

// Fase 9 (v6): blob v5 com tamanho errado (177) é rejeitado.
static void test_migrate_v5_wrongSize_rejected_v6()
{
    IrrigationSettings v5;
    uint8_t raw[IRRIGATION_SETTINGS_V5_SIZE + 1];
    memcpy(raw, &v5, IRRIGATION_SETTINGS_V5_SIZE);
    uint16_t ver5 = 5;
    memcpy(raw + 4, &ver5, 2);
    IrrigationSettings out;
    TEST_ASSERT_FALSE(migrateIrrigationSettings(raw, IRRIGATION_SETTINGS_V5_SIZE + 1, out));
}

static void test_v4_blob_migrates_to_v5()
{
    // Um blob v4 (128 B, version=4) migra: prefixo preservado, regras locais zeradas.
    IrrigationSettings v4;
    v4.version = 4;
    v4.numValves = 3;
    v4.sensores[0].pino = 34;
    uint8_t raw[128];
    memcpy(raw, &v4, 128);
    uint16_t ver = 4; memcpy(raw + 4, &ver, 2); // garante version=4 no blob
    IrrigationSettings out;
    TEST_ASSERT_TRUE(migrateIrrigationSettings(raw, 128, out));
    TEST_ASSERT_EQUAL_UINT16(6, out.version);
    TEST_ASSERT_EQUAL_UINT8(3, out.numValves);
    TEST_ASSERT_EQUAL_INT8(34, out.sensores[0].pino);
    TEST_ASSERT_EQUAL_UINT8(0, out.localInterlocks[0].sensorIdx);
    TEST_ASSERT_EQUAL_UINT8(0, out.localInterlocks[0].saidasValvMask); // inativo
    TEST_ASSERT_EQUAL_UINT8(0, out.localInterlocks[0].saidasGpoMask);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_migrate_v1_preservesFieldsAndDefaultsNew);
    RUN_TEST(test_migrate_v1_getsDefaultButtonLedPins);
    RUN_TEST(test_migrate_v2_getsDefaultButtonLedPins);
    RUN_TEST(test_migrate_v2_preservesFieldsResetsPins);
    RUN_TEST(test_migrate_v3_passthrough);
    RUN_TEST(test_migrate_v3_passthroughKeepsPins);
    RUN_TEST(test_migrate_rejectsBadInput);
    RUN_TEST(test_migrate_v5_wrongSize_rejected);
    RUN_TEST(test_migrate_v4_wrongSize_rejected);
    RUN_TEST(test_migrate_v2_wrongSize_rejected);
    RUN_TEST(test_migrate_v3_to_v4);
    RUN_TEST(test_v5_roundtrip_and_size);
    RUN_TEST(test_defaultStruct_bytesDeterministic);
    RUN_TEST(test_v5_size_and_offsets);
    RUN_TEST(test_v6_defaults_and_size);
    RUN_TEST(test_migrate_v5_to_v6_fills_defaults);
    RUN_TEST(test_migrate_v5_wrongSize_rejected_v6);
    RUN_TEST(test_v4_blob_migrates_to_v5);
    exit(UNITY_END());
}

void loop() {}
