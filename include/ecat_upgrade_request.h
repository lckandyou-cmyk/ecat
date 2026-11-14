/** Request identity shared by upgrade transports; never contains addresses. */
#ifndef ECAT_UPGRADE_REQUEST_H
#define ECAT_UPGRADE_REQUEST_H

#include <stdint.h>

typedef struct {
    uint16_t target_id;
    uint32_t fw_version;
} ecat_upgrade_request_t;

#endif
