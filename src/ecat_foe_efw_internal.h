#ifndef ECAT_FOE_EFW_INTERNAL_H
#define ECAT_FOE_EFW_INTERNAL_H

#include "ecat_foe_efw.h"

/* Validate a header against the locally resolved target, not an ID-indexed array.
 * A NULL target means that the ID has no local registration. */
ecat_foe_efw_err_t ecat_foe_efw_validate_selected(
    const ecat_foe_efw_header_t *hdr, const ecat_foe_efw_config_t *cfg,
    uint16_t selected_id, const ecat_foe_efw_target_cfg_t *target);

#endif
