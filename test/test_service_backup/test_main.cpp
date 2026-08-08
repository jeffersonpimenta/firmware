#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/IrrigationWebApi.h"
#include "modules/irrigation/RemoteButtonTable.h"
#include "modules/irrigation/ServiceBackup.h"
#include <cstring>
#include <unity.h>

using namespace IrrigationService;

void setUp(void) {}
void tearDown(void) {}

static void test_b64_roundtrip()
{
    const uint8_t raw[] = {0xd4, 0xf1, 0xbb, 0x3a};
    char out[16];
    size_t n = base64Encode(raw, 4, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(8, n);
    out[n] = 0;
    TEST_ASSERT_EQUAL_STRING("1PG7Og==", out);
    uint8_t back[8];
    int m = base64Decode(out, n, back, sizeof back);
    TEST_ASSERT_EQUAL_INT(4, m);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(raw, back, 4);
}

static void test_b64_encode_overflow_returns_zero()
{
    const uint8_t raw[] = {1, 2, 3};
    char out[3];
    TEST_ASSERT_EQUAL_size_t(0, base64Encode(raw, 3, out, sizeof out)); // needs 4
}

static void test_b64_decode_rejects_bad_char()
{
    uint8_t back[8];
    TEST_ASSERT_EQUAL_INT(-1, base64Decode("!!!!", 4, back, sizeof back));
}

static void test_jsonMember_nested_object()
{
    const char *o = "{\"a\":1,\"b\":{\"x\":2},\"c\":\"hi\"}";
    Slice v;
    TEST_ASSERT_TRUE(jsonMember(o, strlen(o), "b", v));
    TEST_ASSERT_EQUAL_size_t(7, v.n);
    TEST_ASSERT_EQUAL_MEMORY("{\"x\":2}", v.p, 7);
    TEST_ASSERT_TRUE(jsonMember(o, strlen(o), "c", v));
    TEST_ASSERT_EQUAL_MEMORY("\"hi\"", v.p, 4);
    TEST_ASSERT_FALSE(jsonMember(o, strlen(o), "z", v));
}

struct CountCtx {
    int n;
};
static bool countElem(void *c, Slice)
{
    ((CountCtx *)c)->n++;
    return true;
}

static void test_forEachArray_counts_objects_with_commas_inside()
{
    const char *a = "[{\"x\":1},{\"y\":\"a,b\"},{\"z\":3}]";
    Slice s{a, strlen(a)};
    CountCtx c{0};
    TEST_ASSERT_TRUE(jsonForEachArray(s, &c, countElem));
    TEST_ASSERT_EQUAL_INT(3, c.n);
}

static void test_scalar_getters()
{
    const char *o = "{\"canal\":{\"nome\":\"bv-irrig\"},\"n\":17}";
    Slice root{o, strlen(o)};
    int64_t iv = 0;
    TEST_ASSERT_TRUE(jsonInt(root, "n", iv));
    TEST_ASSERT_EQUAL_INT64(17, iv);
    Slice canal;
    TEST_ASSERT_TRUE(jsonMember(o, strlen(o), "canal", canal));
    char name[16];
    TEST_ASSERT_TRUE(jsonStr(canal, "nome", name, sizeof name));
    TEST_ASSERT_EQUAL_STRING("bv-irrig", name);
}

static const char *kEnv =
    "{\"fmt\":\"irrig-vault\",\"version\":1,\"clients\":["
    "{\"id\":\"f1\",\"nome\":\"A\",\"canal\":{\"nome\":\"c1\",\"psk_b64\":\"1PG7Og==\",\"modem_preset\":\"LONG_FAST\"},\"gateway\":\"!a1b2c3d4\",\"estacoes\":[]},"
    "{\"id\":\"f2\",\"nome\":\"B\",\"canal\":{\"nome\":\"c2\",\"psk_b64\":\"1PG7Og==\",\"modem_preset\":\"LONG_FAST\"},\"gateway\":\"!ffffffff\",\"estacoes\":[]}"
    "]}";

static bool collectId(void *c, Slice cl)
{
    auto *n = (int *)c;
    char id[32];
    if (jsonStr(cl, "id", id, sizeof id))
        (*n)++;
    return true;
}

static void test_envelope_iterates_two_clients()
{
    int n = 0;
    TEST_ASSERT_TRUE(envelopeForEachClient(kEnv, strlen(kEnv), &n, collectId));
    TEST_ASSERT_EQUAL_INT(2, n);
}

static void test_validate_ok()
{
    char err[48] = {0};
    TEST_ASSERT_TRUE(validateEnvelope(kEnv, strlen(kEnv), err, sizeof err));
}

static void test_validate_rejects_bad_fmt()
{
    const char *bad = "{\"fmt\":\"nope\",\"version\":1,\"clients\":[]}";
    char err[48] = {0};
    TEST_ASSERT_FALSE(validateEnvelope(bad, strlen(bad), err, sizeof err));
    TEST_ASSERT_TRUE(err[0] != 0);
}

static void test_validate_rejects_bad_psk()
{
    const char *bad = "{\"fmt\":\"irrig-vault\",\"version\":1,\"clients\":["
                      "{\"id\":\"f1\",\"canal\":{\"psk_b64\":\"!!!!\"},\"gateway\":\"!a1\"}]}";
    char err[48] = {0};
    TEST_ASSERT_FALSE(validateEnvelope(bad, strlen(bad), err, sizeof err));
}

static void test_extractLight_fields()
{
    const char *cl = "{\"id\":\"fazenda-sp-01\",\"nome\":\"Boa Vista\","
                     "\"canal\":{\"nome\":\"bv-irrig\",\"psk_b64\":\"1PG7Og==\",\"modem_preset\":\"LONG_FAST\"},"
                     "\"gateway\":\"!a1b2c3d4\","
                     "\"estacoes\":[{\"no\":\"!e5f6a7b8\",\"nome\":\"Pasto\",\"lat\":-221000000,\"lon\":-476000000}]}";
    Slice s{cl, strlen(cl)};
    LightProfile p;
    TEST_ASSERT_TRUE(extractLight(s, p));
    TEST_ASSERT_EQUAL_STRING("fazenda-sp-01", p.id);
    TEST_ASSERT_EQUAL_STRING("bv-irrig", p.canalNome);
    TEST_ASSERT_EQUAL_UINT8(0, p.preset); // LONG_FAST
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, p.gateway);
    TEST_ASSERT_EQUAL_UINT8(1, p.estacaoCount);
    TEST_ASSERT_EQUAL_HEX32(0xe5f6a7b8, p.estacoes[0].node);
    TEST_ASSERT_EQUAL_INT32(-221000000, p.estacoes[0].lat);
}

// Fase 9: backup estende estacoes[] com config v6 (hbMinutes/limiares). O importador
// faz key-seek e deve IGNORAR as chaves novas, preservando node/lat/lon.
static void test_extractLight_toleratesV6StationFields()
{
    const char *cl = "{\"id\":\"f9\",\"nome\":\"V6\","
                     "\"canal\":{\"nome\":\"c\",\"psk_b64\":\"1PG7Og==\",\"modem_preset\":\"LONG_FAST\"},"
                     "\"gateway\":\"!a1b2c3d4\","
                     "\"estacoes\":[{\"no\":\"!e5f6a7b8\",\"nome\":\"Pasto\",\"lat\":-221000000,\"lon\":-476000000,"
                     "\"hbMinutes\":15,\"vbatAvisoCentiV\":1220,\"vbatCriticaCentiV\":1180}]}";
    Slice s{cl, strlen(cl)};
    LightProfile p;
    TEST_ASSERT_TRUE(extractLight(s, p));
    TEST_ASSERT_EQUAL_UINT8(1, p.estacaoCount);
    TEST_ASSERT_EQUAL_HEX32(0xe5f6a7b8, p.estacoes[0].node);
    TEST_ASSERT_EQUAL_INT32(-221000000, p.estacoes[0].lat);
    TEST_ASSERT_EQUAL_INT32(-476000000, p.estacoes[0].lon);
}

static void test_preset_roundtrip()
{
    TEST_ASSERT_EQUAL_UINT8(1, presetFromString("LONG_SLOW"));
    TEST_ASSERT_EQUAL_STRING("LONG_FAST", presetToString(0));
    TEST_ASSERT_EQUAL_UINT8(0, presetFromString("garbage")); // default
}

static void test_buildClientBackup_roundtrips_through_extractLight()
{
    BackupSource s{};
    s.id = "f1";
    s.nome = "A";
    s.canalNome = "c1";
    s.pskB64 = "1PG7Og==";
    s.preset = 0;
    s.gateway = 0xa1b2c3d4;
    s.estacoesJson = "[{\"no\":\"!e5f6a7b8\",\"nome\":\"P\",\"lat\":10,\"lon\":20}]";
    s.snapshotEpochJson = "{\"!e5f6a7b8\":17}";
    char buf[1024];
    size_t n = buildClientBackup(s, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    Slice cl{buf, n};
    LightProfile p;
    TEST_ASSERT_TRUE(extractLight(cl, p));
    TEST_ASSERT_EQUAL_STRING("f1", p.id);
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, p.gateway);
    TEST_ASSERT_EQUAL_HEX32(0xe5f6a7b8, p.estacoes[0].node);
    Slice cfg;
    TEST_ASSERT_TRUE(jsonMember(buf, n, "config", cfg));
    TEST_ASSERT_TRUE(cfg.n > 0);
}

static void test_buildClientBackup_carries_niveis()
{
    BackupSource s{};
    s.id = "f1";
    s.nome = "A";
    s.canalNome = "c1";
    s.pskB64 = "1PG7Og==";
    s.preset = 0;
    s.gateway = 0xa1b2c3d4;
    s.estacoesJson = "[]";
    s.snapshotEpochJson = "{}";
    s.zonasJson = "[]";
    s.programasJson = "[]";
    s.intertravamentosJson = "[]";
    s.gruposJson = "[]";
    s.sensorNamesJson = "[]";
    s.niveisJson = "[{\"id\":1,\"targetZoneId\":5}]";
    s.seqJson = "";
    char buf[1024];
    size_t n = buildClientBackup(s, buf, sizeof buf);
    TEST_ASSERT_TRUE(n > 0);
    IrrigationService::Slice cfg;
    TEST_ASSERT_TRUE(jsonMember(buf, n, "config", cfg));
    IrrigationService::Slice niveis;
    TEST_ASSERT_TRUE(jsonMember(cfg.p, cfg.n, "niveis", niveis));
    TEST_ASSERT_TRUE(niveis.n > 2); // não é "[]"
}

struct RmCtx {
    char removed[8][32];
    int n;
};
static void rmCb(void *c, const char *id)
{
    auto *r = (RmCtx *)c;
    strncpy(r->removed[r->n++], id, 31);
}

static void test_planMerge_keeps_others_when_not_replace()
{
    const char *ex[] = {"a", "b", "c"};
    const char *in[] = {"b"};
    RmCtx r{};
    planMerge(ex, 3, in, 1, /*replace*/ false, &r, rmCb);
    TEST_ASSERT_EQUAL_INT(0, r.n); // merge: nothing removed
}

static void test_planMerge_replace_removes_absent()
{
    const char *ex[] = {"a", "b", "c"};
    const char *in[] = {"b"};
    RmCtx r{};
    planMerge(ex, 3, in, 1, /*replace*/ true, &r, rmCb);
    TEST_ASSERT_EQUAL_INT(2, r.n); // a and c removed
}

static void test_import_remoteButtons()
{
    const char *env = "{\"clients\":[{\"config\":{\"remoteButtons\":["
        "{\"id\":1,\"enabled\":1,\"targetZoneId\":5,"
        "\"triggers\":[{\"node\":170,\"inputIdx\":2,\"ledSlot\":0}]}]}}]}";
    RemoteButtonTable t;
    size_t n = IrrigationWeb::importRemoteButtonsFromBackup(env, strlen(env), t);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_NOT_NULL(t.byId(1));
    TEST_ASSERT_EQUAL_UINT8(5, t.byId(1)->targetZoneId);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_b64_roundtrip);
    RUN_TEST(test_b64_encode_overflow_returns_zero);
    RUN_TEST(test_b64_decode_rejects_bad_char);
    RUN_TEST(test_jsonMember_nested_object);
    RUN_TEST(test_forEachArray_counts_objects_with_commas_inside);
    RUN_TEST(test_scalar_getters);
    RUN_TEST(test_envelope_iterates_two_clients);
    RUN_TEST(test_validate_ok);
    RUN_TEST(test_validate_rejects_bad_fmt);
    RUN_TEST(test_validate_rejects_bad_psk);
    RUN_TEST(test_extractLight_fields);
    RUN_TEST(test_extractLight_toleratesV6StationFields);
    RUN_TEST(test_preset_roundtrip);
    RUN_TEST(test_buildClientBackup_roundtrips_through_extractLight);
    RUN_TEST(test_buildClientBackup_carries_niveis);
    RUN_TEST(test_planMerge_keeps_others_when_not_replace);
    RUN_TEST(test_planMerge_replace_removes_absent);
    RUN_TEST(test_import_remoteButtons);
    exit(UNITY_END());
}

void loop() {}
