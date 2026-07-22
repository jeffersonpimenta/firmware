#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/GatewayTables.h"
#include "modules/irrigation/IrrigationSettings.h"
#include "modules/irrigation/StationTelemetryCache.h"
#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static Zone mkZone(uint8_t id, uint32_t node, int8_t fonte = -1)
{
    Zone z;
    z.id = id;
    snprintf(z.name, sizeof(z.name), "Z%u", id);
    z.node = node;
    z.maxMin = 45;
    z.padraoMin = 20;
    z.fonteInput = fonte;
    return z;
}

static void test_zones_upsertByIdAndLookup()
{
    ZoneTable t;
    TEST_ASSERT_TRUE(t.upsert(mkZone(1, 0x11)));
    TEST_ASSERT_TRUE(t.upsert(mkZone(2, 0x22, 0)));
    TEST_ASSERT_EQUAL_UINT(2, t.count());

    Zone z1b = mkZone(1, 0x33); // mesmo id: substitui
    TEST_ASSERT_TRUE(t.upsert(z1b));
    TEST_ASSERT_EQUAL_UINT(2, t.count());
    TEST_ASSERT_EQUAL_HEX32(0x33, t.byId(1)->node);

    TEST_ASSERT_NOT_NULL(t.byFonte(0));
    TEST_ASSERT_EQUAL_UINT8(2, t.byFonte(0)->id);
    TEST_ASSERT_NULL(t.byFonte(3));
    TEST_ASSERT_NULL(t.byId(99));

    TEST_ASSERT_TRUE(t.removeById(1));
    TEST_ASSERT_NULL(t.byId(1));
    TEST_ASSERT_FALSE(t.removeById(1));
}

static void test_zones_fullRejects_andIdZeroRejected()
{
    ZoneTable t;
    for (uint8_t i = 1; i <= ZoneTable::MAX; i++)
        TEST_ASSERT_TRUE(t.upsert(mkZone(i, i)));
    TEST_ASSERT_FALSE(t.upsert(mkZone(200, 200)));
    TEST_ASSERT_FALSE(t.upsert(mkZone(0, 1))); // id 0 = inválido
}

static void test_zones_serializeRoundTrip()
{
    ZoneTable t;
    t.upsert(mkZone(1, 0x11));
    t.upsert(mkZone(7, 0x77, 2));
    uint8_t buf[1024];
    size_t n = t.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    ZoneTable c;
    TEST_ASSERT_TRUE(c.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(2, c.count());
    TEST_ASSERT_EQUAL_INT8(2, c.byId(7)->fonteInput);
    buf[0] ^= 0xFF;
    ZoneTable bad;
    TEST_ASSERT_FALSE(bad.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT(0, bad.count());
}

static void test_stations_upsertAdoptAndRoundTrip()
{
    StationRegistry r;
    StationEntry e;
    e.node = 0xa1b2c3d4;
    snprintf(e.name, sizeof(e.name), "Pasto");
    e.desiredEpoch = 5;
    memset(e.blob, 0xAB, sizeof(e.blob));
    TEST_ASSERT_TRUE(r.upsert(e));
    TEST_ASSERT_EQUAL_UINT32(5, r.byNode(0xa1b2c3d4)->desiredEpoch);

    // Monta blob v4 válido (128 B) para adoptConfig migrar p/ v5 canônico (176 B).
    uint8_t newBlob[sizeof(StationEntry::blob)];
    memset(newBlob, 0, sizeof(newBlob));
    IrrigationSettings v4src;
    v4src.version = 4;
    v4src.numValves = 3;
    v4src.configEpoch = 9;
    memcpy(newBlob, &v4src, IRRIGATION_SETTINGS_V4_SIZE);
    uint16_t ver4 = 4;
    memcpy(newBlob + 4, &ver4, 2); // garante version=4 no blob

    r.adoptConfig(0xa1b2c3d4, newBlob, IRRIGATION_SETTINGS_V4_SIZE, 9); // regra do maior epoch
    TEST_ASSERT_EQUAL_UINT32(9, r.byNode(0xa1b2c3d4)->desiredEpoch);
    // Após migração p/ v5, magic deve estar nos primeiros 4 bytes do blob
    uint32_t blobMagic = 0;
    memcpy(&blobMagic, r.byNode(0xa1b2c3d4)->blob, 4);
    TEST_ASSERT_EQUAL_HEX32(IrrigationSettings::MAGIC, blobMagic);
    // Confirmação de cauda: blob[175] deve sobreviver ao roundtrip (cobre os 176 B)
    TEST_ASSERT_EQUAL_UINT8(0, r.byNode(0xa1b2c3d4)->blob[175]);

    uint8_t buf[2048];
    size_t n = r.serialize(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    StationRegistry c;
    TEST_ASSERT_TRUE(c.deserialize(buf, n));
    TEST_ASSERT_EQUAL_UINT32(9, c.byNode(0xa1b2c3d4)->desiredEpoch);
    TEST_ASSERT_EQUAL_STRING("Pasto", c.byNode(0xa1b2c3d4)->name);
    // Cauda do blob preservada após serializar/desserializar
    TEST_ASSERT_EQUAL_UINT8(0, c.byNode(0xa1b2c3d4)->blob[175]);
}

static void test_stations_fullAndNodeZeroRejected()
{
    StationRegistry r;
    for (uint32_t i = 1; i <= StationRegistry::MAX; i++) {
        StationEntry e;
        e.node = i;
        TEST_ASSERT_TRUE(r.upsert(e));
    }
    StationEntry x;
    x.node = 999;
    TEST_ASSERT_FALSE(r.upsert(x));
    StationEntry z; // node 0
    TEST_ASSERT_FALSE(r.upsert(z));
    TEST_ASSERT_TRUE(r.removeByNode(3));
    TEST_ASSERT_TRUE(r.upsert(x));
}

static void test_stations_nodeAtCompacted()
{
    StationRegistry r;
    StationEntry a;
    a.node = 0xAA;
    snprintf(a.name, sizeof(a.name), "A");
    StationEntry b;
    b.node = 0xBB;
    snprintf(b.name, sizeof(b.name), "B");
    TEST_ASSERT_TRUE(r.upsert(a));
    TEST_ASSERT_TRUE(r.upsert(b));
    TEST_ASSERT_TRUE(r.removeByNode(0xAA)); // abre buraco no slot 0
    TEST_ASSERT_EQUAL_UINT(1, r.count());
    const StationEntry *e0 = r.nodeAt(0);
    TEST_ASSERT_NOT_NULL(e0);
    TEST_ASSERT_EQUAL_HEX32(0xBB, e0->node); // compactado: pula o buraco
    TEST_ASSERT_NULL(r.nodeAt(1));           // fora do count
    TEST_ASSERT_NULL(r.nodeAt(99));
}

static void test_telemetryCache_upsertAndLookup()
{
    StationTelemetryCache c;
    StationTelemetry t = {};
    t.node = 0x55;
    t.vbatCentiV = 1230;
    t.vpanelCentiV = 1810;
    t.snrQuarterDb = 24;
    t.rebootCount = 4;
    t.flags = 0x02;
    t.configEpoch = 9;
    t.atMs = 1000;
    c.update(t);
    const StationTelemetry *got = c.byNode(0x55);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_UINT16(1230, got->vbatCentiV);
    TEST_ASSERT_EQUAL_UINT32(9, got->configEpoch);
    // update do mesmo nó sobrescreve
    t.vbatCentiV = 1200;
    t.atMs = 2000;
    c.update(t);
    TEST_ASSERT_EQUAL_UINT16(1200, c.byNode(0x55)->vbatCentiV);
    TEST_ASSERT_NULL(c.byNode(0x99));
}

static void test_cache_guarda_sensores_e_tamper()
{
    StationTelemetryCache c;
    StationTelemetry t; t.node = 0xAA; t.sensorCount = 2; t.tamper = true;
    t.sensors[0].id = 0; t.sensors[0].tipo = 1; t.sensors[0].valueCenti = 250;
    c.update(t);
    const StationTelemetry *g = c.byNode(0xAA);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_UINT8(2, g->sensorCount);
    TEST_ASSERT_TRUE(g->tamper);
    TEST_ASSERT_EQUAL_INT16(250, g->sensors[0].valueCenti);
}

static void test_entry_at_itera_ocupados()
{
    StationTelemetryCache c;
    StationTelemetry t; t.node = 0xBB; c.update(t);
    // entryAt cobre TODOS os slots (0..MAX-1); node==0 = vazio.
    bool achou = false;
    for (size_t i = 0; i < StationTelemetryCache::MAX; i++) {
        const StationTelemetry *e = c.entryAt(i);
        if (e && e->node == 0xBB) achou = true;
    }
    TEST_ASSERT_TRUE(achou);
    TEST_ASSERT_NULL(c.entryAt(StationTelemetryCache::MAX)); // fora de faixa
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_zones_upsertByIdAndLookup);
    RUN_TEST(test_zones_fullRejects_andIdZeroRejected);
    RUN_TEST(test_zones_serializeRoundTrip);
    RUN_TEST(test_stations_upsertAdoptAndRoundTrip);
    RUN_TEST(test_stations_fullAndNodeZeroRejected);
    RUN_TEST(test_stations_nodeAtCompacted);
    RUN_TEST(test_telemetryCache_upsertAndLookup);
    RUN_TEST(test_cache_guarda_sensores_e_tamper);
    RUN_TEST(test_entry_at_itera_ocupados);
    exit(UNITY_END());
}

void loop() {}
