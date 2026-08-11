#include "Arduino.h"
#include "TestUtil.h"
#include "modules/irrigation/AccessWindowPolicy.h"
#include "modules/irrigation/IrrigationSettings.h"
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

using AccessWindowPolicy::ButtonAction;

// ---------------------------------------------------------------------------
// eligible: só ESTACAO/REPETIDOR provisionados; GATEWAY/SERVICO nunca; não
// provisionado nunca.
// ---------------------------------------------------------------------------

static void test_eligible_estacao_provisioned()
{
    TEST_ASSERT_TRUE(AccessWindowPolicy::eligible(IrrigationRole::ESTACAO, true));
}

static void test_eligible_repetidor_provisioned()
{
    TEST_ASSERT_TRUE(AccessWindowPolicy::eligible(IrrigationRole::REPETIDOR, true));
}

static void test_eligible_estacao_not_provisioned()
{
    TEST_ASSERT_FALSE(AccessWindowPolicy::eligible(IrrigationRole::ESTACAO, false));
}

static void test_eligible_repetidor_not_provisioned()
{
    TEST_ASSERT_FALSE(AccessWindowPolicy::eligible(IrrigationRole::REPETIDOR, false));
}

static void test_eligible_gateway_never()
{
    TEST_ASSERT_FALSE(AccessWindowPolicy::eligible(IrrigationRole::GATEWAY, true));
    TEST_ASSERT_FALSE(AccessWindowPolicy::eligible(IrrigationRole::GATEWAY, false));
}

static void test_eligible_servico_never()
{
    TEST_ASSERT_FALSE(AccessWindowPolicy::eligible(IrrigationRole::SERVICO, true));
    TEST_ASSERT_FALSE(AccessWindowPolicy::eligible(IrrigationRole::SERVICO, false));
}

// ---------------------------------------------------------------------------
// shouldTearDownBle = eligible && windowWasOpen && !windowOpenNow && !bleAlreadyReleased.
// Só a borda OPEN->CLOSED de nó elegível ainda com BLE dispara.
// ---------------------------------------------------------------------------

static void test_teardown_edge_open_to_closed()
{
    // eligible, was open, now closed, not released -> true
    TEST_ASSERT_TRUE(AccessWindowPolicy::shouldTearDownBle(true, false, true, false));
}

static void test_teardown_not_eligible()
{
    TEST_ASSERT_FALSE(AccessWindowPolicy::shouldTearDownBle(false, false, true, false));
}

static void test_teardown_still_open()
{
    // windowOpenNow == true -> não é borda de fechamento
    TEST_ASSERT_FALSE(AccessWindowPolicy::shouldTearDownBle(true, true, true, false));
}

static void test_teardown_was_already_closed()
{
    // windowWasOpen == false -> nenhuma borda (permanece fechado)
    TEST_ASSERT_FALSE(AccessWindowPolicy::shouldTearDownBle(true, false, false, false));
}

static void test_teardown_already_released()
{
    // bleAlreadyReleased == true -> não repete o release
    TEST_ASSERT_FALSE(AccessWindowPolicy::shouldTearDownBle(true, false, true, true));
}

static void test_teardown_opening_edge()
{
    // borda CLOSED->OPEN (abertura): jamais derruba
    TEST_ASSERT_FALSE(AccessWindowPolicy::shouldTearDownBle(true, true, false, false));
}

// ---------------------------------------------------------------------------
// buttonShortAction: !eligible->NONE; eligible && !released->REOPEN_LIVE;
// eligible && released->REBOOT_TO_REOPEN.
// ---------------------------------------------------------------------------

static void test_button_not_eligible_none()
{
    TEST_ASSERT_EQUAL(ButtonAction::NONE, AccessWindowPolicy::buttonShortAction(false, false));
    TEST_ASSERT_EQUAL(ButtonAction::NONE, AccessWindowPolicy::buttonShortAction(false, true));
}

static void test_button_ble_live_reopen()
{
    TEST_ASSERT_EQUAL(ButtonAction::REOPEN_LIVE, AccessWindowPolicy::buttonShortAction(true, false));
}

static void test_button_ble_released_reboot()
{
    TEST_ASSERT_EQUAL(ButtonAction::REBOOT_TO_REOPEN, AccessWindowPolicy::buttonShortAction(true, true));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_eligible_estacao_provisioned);
    RUN_TEST(test_eligible_repetidor_provisioned);
    RUN_TEST(test_eligible_estacao_not_provisioned);
    RUN_TEST(test_eligible_repetidor_not_provisioned);
    RUN_TEST(test_eligible_gateway_never);
    RUN_TEST(test_eligible_servico_never);
    RUN_TEST(test_teardown_edge_open_to_closed);
    RUN_TEST(test_teardown_not_eligible);
    RUN_TEST(test_teardown_still_open);
    RUN_TEST(test_teardown_was_already_closed);
    RUN_TEST(test_teardown_already_released);
    RUN_TEST(test_teardown_opening_edge);
    RUN_TEST(test_button_not_eligible_none);
    RUN_TEST(test_button_ble_live_reopen);
    RUN_TEST(test_button_ble_released_reboot);
    exit(UNITY_END());
}

void loop() {}
