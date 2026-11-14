/** FoE filename/EOF bridge into a shared transport-independent upgrade manager. */
#ifndef ECAT_FOE_TRANSPORT_H
#define ECAT_FOE_TRANSPORT_H
#include "ecat_upgrade.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ecat_upgrade_t *upgrade;
    uint64_t token;
    bool pending;
    bool finish_after_write;
    bool finishing;
} ecat_foe_transport_t;

/* Initialize once; address is the stable owner identity, so do not copy it. */
int ecat_foe_transport_init(ecat_foe_transport_t *transport, ecat_upgrade_t *upgrade);
/* Returns FoE wire codes; a new WRQ can replace only this bridge's session. */
int ecat_foe_transport_begin(ecat_foe_transport_t *transport, const char *name, size_t length);
ecat_upgrade_async_result_t ecat_foe_transport_start(void *ctx, const uint8_t *data,
    size_t length, bool more_data);
ecat_upgrade_async_result_t ecat_foe_transport_poll(void *ctx);
void ecat_foe_transport_abort(void *ctx);
bool ecat_foe_transport_owns(const ecat_foe_transport_t *transport);
/* Suitable for the existing SSC retry gate; final packet waits for commit. */
const ecat_upgrade_async_ops_t *ecat_foe_transport_async_ops(void);
#ifdef __cplusplus
}
#endif
#endif
