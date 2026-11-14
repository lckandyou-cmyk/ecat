#include "ecat_foe_driver_template.h"
#include <stdio.h>
#include <stdint.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    ecat_foe_driver_template_ctx_t ctx = {0};
    const ecat_foe_target_driver_t *ops = ecat_foe_driver_template_get_ops();
    uint32_t end = 0U;
    ctx.max_size = 8192U;
    ctx.sector_size = 4096U;
    CHECK(ops->init(&ctx) == 0);
    CHECK(ops->erase_to(1U, &end, &ctx) != 0);
    CHECK(ops->invalidate(&ctx) == 0);
    CHECK(ops->erase_to(1U, &end, &ctx) == 0 && end == 4096U);
    ctx.max_size = UINT32_MAX;
    ctx.erased_watermark = UINT32_MAX - 10U;
    ctx.sector_size = 16U;
    CHECK(ops->erase_to(UINT32_MAX, &end, &ctx) != 0);
    puts("PASS: driver template invalidation and erase bounds");
    return 0;
}
