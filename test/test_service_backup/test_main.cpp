#include "Arduino.h"
#include "TestUtil.h"
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
    exit(UNITY_END());
}

void loop() {}
