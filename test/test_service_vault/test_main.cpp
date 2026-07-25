#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/ServiceBackup.h"
#include "modules/irrigation/ServiceVault.h"
#include "support/RamProfileStore.h"
#include <cstring>
#include <unity.h>

using namespace IrrigationService;

void setUp(void) {}
void tearDown(void) {}

static const char *kEnv2 =
    "{\"fmt\":\"irrig-vault\",\"version\":1,\"clients\":["
    "{\"id\":\"f1\",\"nome\":\"A\",\"canal\":{\"nome\":\"c1\",\"psk_b64\":\"1PG7Og==\",\"modem_preset\":\"LONG_FAST\"},\"gateway\":\"!a1b2c3d4\",\"estacoes\":[]},"
    "{\"id\":\"f2\",\"nome\":\"B\",\"canal\":{\"nome\":\"c2\",\"psk_b64\":\"1PG7Og==\",\"modem_preset\":\"LONG_FAST\"},\"gateway\":\"!ffffffff\",\"estacoes\":[]}"
    "]}";

static void test_ramstore_write_read_list()
{
    RamProfileStore s;
    TEST_ASSERT_TRUE(s.writeProfile("f1", "{\"id\":\"f1\"}", 11));
    char buf[64];
    size_t n = 0;
    TEST_ASSERT_TRUE(s.readProfile("f1", buf, sizeof buf, n));
    TEST_ASSERT_EQUAL_size_t(11, n);
    char ids[8][32];
    TEST_ASSERT_EQUAL_size_t(1, s.listIds(ids, 8));
    TEST_ASSERT_EQUAL_STRING("f1", ids[0]);
}

static void test_ramstore_active_roundtrip()
{
    RamProfileStore s;
    TEST_ASSERT_TRUE(s.setActive("f2"));
    char a[32];
    TEST_ASSERT_TRUE(s.getActive(a, sizeof a));
    TEST_ASSERT_EQUAL_STRING("f2", a);
}

static void test_vault_import_lists_two()
{
    RamProfileStore s;
    ServiceVault v(s);
    char err[48];
    TEST_ASSERT_TRUE(v.importEnvelope(kEnv2, strlen(kEnv2), false, err, sizeof err));
    LightProfile lp[8];
    TEST_ASSERT_EQUAL_size_t(2, v.listClients(lp, 8));
}

static void test_vault_select_sets_active()
{
    RamProfileStore s;
    ServiceVault v(s);
    char err[48];
    v.importEnvelope(kEnv2, strlen(kEnv2), false, err, sizeof err);
    TEST_ASSERT_TRUE(v.select("f2"));
    char a[32];
    TEST_ASSERT_TRUE(v.activeId(a, sizeof a));
    TEST_ASSERT_EQUAL_STRING("f2", a);
    TEST_ASSERT_FALSE(v.select("nope"));
}

static void test_vault_seq_unknown_then_set()
{
    RamProfileStore s;
    ServiceVault v(s);
    char err[48];
    v.importEnvelope(kEnv2, strlen(kEnv2), false, err, sizeof err);
    TEST_ASSERT_FALSE(v.hasSeq("f1", 0xe5f6a7b8));
    TEST_ASSERT_EQUAL_UINT32(0, v.seqFor("f1", 0xe5f6a7b8));
    v.setSeq("f1", 0xe5f6a7b8, 4213);
    TEST_ASSERT_TRUE(v.hasSeq("f1", 0xe5f6a7b8));
    TEST_ASSERT_EQUAL_UINT32(4213, v.seqFor("f1", 0xe5f6a7b8));
}

static void test_vault_seq_two_nodes_independent()
{
    RamProfileStore s;
    ServiceVault v(s);
    char err[48];
    v.importEnvelope(kEnv2, strlen(kEnv2), false, err, sizeof err);
    v.setSeq("f1", 0x11, 10);
    v.setSeq("f1", 0x22, 20);
    TEST_ASSERT_EQUAL_UINT32(10, v.seqFor("f1", 0x11));
    TEST_ASSERT_EQUAL_UINT32(20, v.seqFor("f1", 0x22));
}

static void test_vault_export_roundtrips_import()
{
    RamProfileStore s;
    ServiceVault v(s);
    char err[48];
    v.importEnvelope(kEnv2, strlen(kEnv2), false, err, sizeof err);
    char out[4096];
    size_t n = v.exportEnvelope(out, sizeof out);
    TEST_ASSERT_TRUE(n > 0);
    RamProfileStore s2;
    ServiceVault v2(s2);
    TEST_ASSERT_TRUE(v2.importEnvelope(out, n, false, err, sizeof err));
    LightProfile lp[8];
    TEST_ASSERT_EQUAL_size_t(2, v2.listClients(lp, 8));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_ramstore_write_read_list);
    RUN_TEST(test_ramstore_active_roundtrip);
    RUN_TEST(test_vault_import_lists_two);
    RUN_TEST(test_vault_select_sets_active);
    RUN_TEST(test_vault_seq_unknown_then_set);
    RUN_TEST(test_vault_seq_two_nodes_independent);
    RUN_TEST(test_vault_export_roundtrips_import);
    exit(UNITY_END());
}
void loop() {}
