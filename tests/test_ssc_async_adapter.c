#include "ecat_foe_ssc.h"
#include "mock_driver.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct {
    unsigned starts;
    unsigned polls;
    unsigned aborts;
} async_mock_t;

static ecat_foe_ssc_async_result_t async_start(void *ctx, const uint8_t *data,
                                                 size_t length, bool following)
{
    async_mock_t *m = (async_mock_t *)ctx;
    (void)data;
    (void)length;
    (void)following;
    m->starts++;
    m->polls = 0U;
    return ECAT_FOE_SSC_ASYNC_PENDING;
}

static ecat_foe_ssc_async_result_t async_poll(void *ctx)
{
    async_mock_t *m = (async_mock_t *)ctx;
    m->polls++;
    return (m->polls == 1U) ? ECAT_FOE_SSC_ASYNC_PENDING : ECAT_FOE_SSC_ASYNC_COMPLETE;
}

static void async_abort(void *ctx)
{
    ((async_mock_t *)ctx)->aborts++;
}

int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
    (void)target_id;
    *floor_ver = 0U;
    return 0;
}

int port_set_version_floor(uint16_t target_id, uint32_t new_floor_ver)
{
    (void)target_id;
    (void)new_floor_ver;
    return 0;
}

int main(void)
{
    static mock_driver_t driver;
    static uint8_t packet_storage[64];
    static const ecat_foe_ssc_async_ops_t ops = {async_start, async_poll, async_abort};
    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    ecat_foe_ssc_async_t gate;
    ecat_foe_ssc_adapter_t adapter;
    async_mock_t async = {0};

    mock_driver_init(&driver);
    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
    cfg.targets[0].target_id = 1U;
    cfg.targets[0].enabled = true;
    cfg.targets[0].max_payload_size = 4096U;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = &driver.driver;
    cfg.targets[0].user_ctx = &driver;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_ssc_async_init(&gate, &ops, &async, packet_storage,
                                  sizeof(packet_storage), 100U,
                                  0x800AU, ECAT_FOE_SSC_ERR_PACKETNO,
                                  ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    ecat_foe_ssc_init_async(&adapter, &core, &gate);

    const char filename[] = "SERVO_DRV_MCU_V1.0.0.0.efw";
    CHECK(ecat_foe_ssc_on_write(&adapter, filename, (uint16_t)(sizeof(filename) - 1U), 0U) == 0U);

    const uint8_t first[] = {1U, 2U, 3U};
    CHECK(ecat_foe_ssc_on_data_following(&adapter, first, sizeof(first), true, 10U) == 0x800AU);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, (uint8_t[]){9U, 2U, 3U}, sizeof(first), true, 10U) == ECAT_FOE_SSC_ERR_PACKETNO);
    CHECK(!gate.pending && !gate.completed && !adapter.session_active && async.aborts == 1U);
    CHECK(core.state == ECAT_FOE_STATE_IDLE);
    CHECK(ecat_foe_ssc_on_main_loop(&adapter, 11U) == ECAT_FOE_SSC_ERR_PACKETNO);
    CHECK(async.polls == 0U && async.aborts == 1U);
    ecat_foe_ssc_on_abort(&adapter, ECAT_FOE_SSC_ERR_PACKETNO);
    CHECK(async.aborts == 1U);
    CHECK(ecat_foe_ssc_on_write(&adapter, filename, (uint16_t)(sizeof(filename) - 1U), 0U) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, first, sizeof(first), true, 11U) == 0x800AU);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, first, sizeof(first), true, 11U) == 0x800AU);
    CHECK(async.starts == 2U);
    CHECK(ecat_foe_ssc_on_main_loop(&adapter, 12U) == 0x800AU);
    CHECK(ecat_foe_ssc_on_main_loop(&adapter, 13U) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, first, sizeof(first), true, 14U) == 0U);
    CHECK(adapter.session_active);

    const uint8_t final[] = {4U, 5U};
    CHECK(ecat_foe_ssc_on_data_following(&adapter, final, sizeof(final), false, 20U) == 0x800AU);
    CHECK(ecat_foe_ssc_on_main_loop(&adapter, 21U) == 0x800AU);
    CHECK(ecat_foe_ssc_on_main_loop(&adapter, 22U) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, final, sizeof(final), false, 23U) == 0U);
    CHECK(!adapter.session_active && adapter.async_final_acked);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, NULL, 0U, false, 24U) == 0U);
    CHECK(async.starts == 3U && async.aborts == 1U);

    /* Invalid input while work is pending also cancels it exactly once. */
    CHECK(ecat_foe_ssc_on_write(&adapter, filename, (uint16_t)(sizeof(filename) - 1U), 0U) == 0U);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, first, sizeof(first), true, 30U) == 0x800AU);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, NULL, 1U, true, 31U) == ECAT_FOE_SSC_ERR_PACKETNO);
    CHECK(!gate.pending && !adapter.session_active && async.aborts == 2U);
    CHECK(core.state == ECAT_FOE_STATE_IDLE);

    puts("PASS: SSC async callback routing, bDataFollowing and main-loop poll");
    return 0;
}
