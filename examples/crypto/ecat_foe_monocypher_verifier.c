#include "ecat_foe_monocypher_verifier.h"

#include <string.h>
#include <monocypher.h>
#include <monocypher-ed25519.h>

static int verifier_begin(const uint8_t key_id[ECAT_FOE_EFW_KEY_ID_LEN],
                          const uint8_t signature[ECAT_FOE_EFW_SIGNATURE_LEN],
                          void *user_ctx)
{
    ecat_foe_monocypher_verifier_t *v = (ecat_foe_monocypher_verifier_t *)user_ctx;
    if (v == NULL || key_id == NULL || signature == NULL || v->keys == NULL || v->key_count == 0U) return -1;
    for (size_t i = 0; i < v->key_count; ++i) {
        if (memcmp(key_id, v->keys[i].key_id, ECAT_FOE_EFW_KEY_ID_LEN) == 0) {
            memcpy(v->active_public_key, v->keys[i].public_key, sizeof(v->active_public_key));
            memcpy(v->signature, signature, sizeof(v->signature));
            crypto_sha512_init(&v->hash);
            v->active = true;
            return 0;
        }
    }
    return -1; /* unknown key is a hard failure; there is no unsigned fallback */
}

static int verifier_update(const uint8_t *data, size_t len, void *user_ctx)
{
    (void)user_ctx;
    ecat_foe_monocypher_verifier_t *v = (ecat_foe_monocypher_verifier_t *)user_ctx;
    if (v == NULL || !v->active || (data == NULL && len != 0U)) return -1;
    crypto_sha512_update(&v->hash, data, len);
    return 0;
}

static int verifier_finish(void *user_ctx)
{
    (void)user_ctx;
    ecat_foe_monocypher_verifier_t *v = (ecat_foe_monocypher_verifier_t *)user_ctx;
    uint8_t digest[64];
    if (v == NULL || !v->active) return -1;
    crypto_sha512_final(&v->hash, digest);
    int result = crypto_ed25519_ph_check(v->signature, v->active_public_key, digest);
    crypto_wipe(digest, sizeof(digest));
    crypto_wipe(&v->hash, sizeof(v->hash));
    crypto_wipe(v->signature, sizeof(v->signature));
    crypto_wipe(v->active_public_key, sizeof(v->active_public_key));
    v->active = false;
    return result;
}

static void verifier_abort(void *user_ctx)
{
    ecat_foe_monocypher_verifier_t *v = (ecat_foe_monocypher_verifier_t *)user_ctx;
    if (v != NULL) {
        crypto_wipe(&v->hash, sizeof(v->hash));
        crypto_wipe(v->signature, sizeof(v->signature));
        crypto_wipe(v->active_public_key, sizeof(v->active_public_key));
        v->active = false;
    }
}

void ecat_foe_monocypher_verifier_init(ecat_foe_monocypher_verifier_t *verifier,
                                       const ecat_foe_trusted_key_t *keys,
                                       size_t key_count,
                                       ecat_foe_signature_verifier_t *callbacks)
{
    if (verifier == NULL || callbacks == NULL) return;
    verifier->keys = keys;
    verifier->key_count = key_count;
    callbacks->begin = verifier_begin;
    callbacks->update = verifier_update;
    callbacks->finish = verifier_finish;
    callbacks->abort = verifier_abort;
    callbacks->user_ctx = verifier;
}
