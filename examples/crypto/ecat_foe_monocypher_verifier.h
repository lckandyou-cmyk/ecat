#ifndef ECAT_FOE_MONOCYPHER_VERIFIER_H
#define ECAT_FOE_MONOCYPHER_VERIFIER_H

#include "ecat_foe_core.h"
#include <monocypher-ed25519.h>

typedef struct {
    uint8_t key_id[ECAT_FOE_EFW_KEY_ID_LEN];
    uint8_t public_key[32];
} ecat_foe_trusted_key_t;

typedef struct {
    const ecat_foe_trusted_key_t *keys;
    size_t key_count;
    crypto_sha512_ctx hash;
    uint8_t signature[ECAT_FOE_EFW_SIGNATURE_LEN];
    uint8_t active_public_key[32];
    bool active;
} ecat_foe_monocypher_verifier_t;

/* Optional adapter; compile only when the application supplies Monocypher 4.0.3. */
void ecat_foe_monocypher_verifier_init(
    ecat_foe_monocypher_verifier_t *verifier,
    const ecat_foe_trusted_key_t *keys,
    size_t key_count,
    ecat_foe_signature_verifier_t *callbacks);

#endif
