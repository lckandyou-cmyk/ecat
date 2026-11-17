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

static ecat_upgrade_async_result_t async_start(void *ctx, const uint8_t *data,
                                                 size_t length, bool following)
{
    async_mock_t *m = (async_mock_t *)ctx;
    (void)data;
    (void)length;
    (void)following;
    m->starts++;
    m->polls = 0U;
    return ECAT_UPGRADE_ASYNC_PENDING;
}

static ecat_upgrade_async_result_t async_poll(void *ctx)
{
    async_mock_t *m = (async_mock_t *)ctx;
    m->polls++;
    return (m->polls == 1U) ? ECAT_UPGRADE_ASYNC_PENDING : ECAT_UPGRADE_ASYNC_COMPLETE;
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
    static const ecat_upgrade_async_ops_t ops = {async_start, async_poll, async_abort};
    ecat_foe_core_t core;
    ecat_foe_core_cfg_t cfg;
    ecat_foe_ssc_async_t gate;
    ecat_foe_ssc_adapter_t adapter;
    async_mock_t async = {0};

    ecat_target_registry_t registry;
    ecat_target_desc_t target = {0};
    mock_driver_init(&driver);
    CHECK(ecat_target_registry_init(&registry) == 0);
    target.target_id = 1U;
    memcpy(target.name, "mcu", sizeof("mcu"));
    target.type = ECAT_TARGET_TYPE_FIRMWARE;
    target.permissions = ECAT_PERM_WRITE;
    target.max_size = 4096U;
    target.write_unit = 8U;
    target.enabled = true;
    target.driver.io = driver.driver;
    target.user_ctx = &driver;
    CHECK(ecat_target_registry_add(&registry, &target) == 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.project_id = 0x10010001U;
    cfg.project_name = "SERVO_DRV";
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = ECAT_FOE_EFW_HEADER_MIN_LEN;
    cfg.registry = &registry;
    CHECK(ecat_foe_core_init(&core, &cfg) == 0);
    CHECK(ecat_foe_ssc_async_init(&gate, &ops, &async, packet_storage,
                                  sizeof(packet_storage), 100U,
                                  0x800AU, ECAT_FOE_SSC_ERR_PACKETNO,
                                  ECAT_FOE_SSC_ERR_PROGERROR) == 0);
    ecat_foe_ssc_init_async(&adapter, &core, &gate);

    /* An arbitrary gate must not bypass the transfer manager or its policy. */
    const char filename[] = "SERVO_DRV_MCU_V1.0.0.0.efw";
    const uint8_t data[] = {1U, 2U, 3U};
    CHECK(ecat_foe_ssc_on_write(&adapter, filename, (uint16_t)(sizeof(filename) - 1U), 0U) == ECAT_FOE_SSC_ERR_ILLEGAL);
    CHECK(ecat_foe_ssc_on_data_following(&adapter, data, sizeof(data), true, 10U) == ECAT_FOE_SSC_ERR_ILLEGAL);
    CHECK(ecat_foe_ssc_on_main_loop(&adapter, 11U) == ECAT_FOE_SSC_ERR_ILLEGAL);
    CHECK(ecat_foe_ssc_on_end(&adapter) == ECAT_FOE_SSC_ERR_PROGERROR);
    ecat_foe_ssc_on_abort(&adapter, ECAT_FOE_SSC_ERR_PACKETNO);
    CHECK(async.starts == 0U && async.polls == 0U && async.aborts == 0U);
    CHECK(core.state == ECAT_FOE_STATE_IDLE && driver.count_init == 0U);

    ecat_transfer_t transfer;
    CHECK(ecat_transfer_init(&transfer, &registry, &core) == 0);
    CHECK(ecat_foe_ssc_bind_transfer(&adapter, &transfer) == -1);
    CHECK(ecat_foe_ssc_on_write(&adapter, filename, (uint16_t)(sizeof(filename) - 1U), 0U) == ECAT_FOE_SSC_ERR_ILLEGAL);
    puts("PASS: SSC rejects unbound and non-manager async gates");
    return 0;
}
