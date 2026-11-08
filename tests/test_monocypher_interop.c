/* Real CLI-to-core Ed25519ph integration; Monocypher is supplied externally. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ecat_foe_core.h"
#include "ecat_foe_monocypher_verifier.h"
#include "mock_driver.h"

#define HEADER_SIZE ECAT_FOE_EFW_HEADER_MIN_LEN
#define BUFFER_SIZE 4096U

int port_get_version_floor(uint16_t target_id, uint32_t *floor_ver)
{
    (void)target_id;
    (void)floor_ver;
    return -1;
}

static size_t read_file(const char *path, uint8_t *buffer, size_t capacity)
{
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    size_t n = fread(buffer, 1, capacity, fp);
    assert(!ferror(fp));
    assert(fgetc(fp) == EOF && feof(fp));
    assert(fclose(fp) == 0);
    return n;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void update_header_crc(uint8_t *package)
{
    put_u32(package + 144U, ecat_foe_crc32_calc(package, 144U));
}

static int receive(const uint8_t *package, size_t size,
                   const ecat_foe_trusted_key_t *key, mock_driver_t *driver,
                   bool corrupt_readback)
{
    ecat_foe_monocypher_verifier_t verifier = {0};
    ecat_foe_signature_verifier_t callbacks = {0};
    ecat_foe_monocypher_verifier_init(&verifier, key, 1U, &callbacks);
    ecat_foe_core_cfg_t cfg = {0};
    cfg.project_name = "INTEROP";
    cfg.project_id = 0x10010001U;
    cfg.hardware_id = 1U;
    cfg.local_hw_ver = 1U;
    cfg.max_header_size = HEADER_SIZE;
    cfg.signature_verify_enable = true;
    cfg.signature_verifier = callbacks;
    cfg.targets[0].enabled = true;
    cfg.targets[0].target_id = 1U;
    cfg.targets[0].max_payload_size = MOCK_DRIVER_MEM_SIZE;
    cfg.targets[0].write_unit = 8U;
    cfg.targets[0].driver = &driver->driver;
    cfg.targets[0].user_ctx = driver;
    ecat_foe_core_t core;
    assert(ecat_foe_core_init(&core, &cfg) == 0);
    const char *name = "INTEROP_MCU_V1.2.3.4.efw";
    assert(ecat_foe_core_on_wrq(&core, name, strlen(name)) == 0);
    driver->inject_corrupt_read = corrupt_readback;
    for (size_t pos = 0; pos < size;) {
        size_t n = (size - pos > 37U) ? 37U : size - pos;
        if (ecat_foe_core_on_data(&core, package + pos, n) != 0) return -1;
        pos += n;
    }
    return ecat_foe_core_on_end(&core);
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    uint8_t original[BUFFER_SIZE], package[BUFFER_SIZE], public_key[32];
    size_t size = read_file(argv[1], original, sizeof(original));
    size_t unsigned_size = read_file(argv[2], package, sizeof(package));
    assert(size > HEADER_SIZE && unsigned_size > HEADER_SIZE);
    assert(read_file(argv[3], public_key, sizeof(public_key)) == sizeof(public_key));
    ecat_foe_trusted_key_t key;
    memcpy(key.key_id, original + 72U, sizeof(key.key_id));
    memcpy(key.public_key, public_key, sizeof(key.public_key));
    static mock_driver_t driver;

    mock_driver_init(&driver);
    assert(receive(original, size, &key, &driver, false) == 0);
    assert(driver.count_finalize == 1U && driver.valid_flag);
    assert(memcmp(driver.mem, original + HEADER_SIZE, size - HEADER_SIZE) == 0);

    mock_driver_init(&driver);
    assert(receive(package, unsigned_size, &key, &driver, false) != 0);
    assert(driver.count_init == 0U && driver.count_finalize == 0U);

    memcpy(package, original, size);
    package[72] ^= 1U;
    update_header_crc(package);
    mock_driver_init(&driver);
    assert(receive(package, size, &key, &driver, false) != 0);
    assert(driver.count_init == 0U && driver.count_finalize == 0U);

    memcpy(package, original, size);
    package[80] ^= 1U;
    update_header_crc(package);
    mock_driver_init(&driver);
    assert(receive(package, size, &key, &driver, false) != 0);
    assert(driver.count_abort == 1U && driver.count_finalize == 0U && !driver.valid_flag);

    memcpy(package, original, size);
    package[18] = 2U; /* Still compatible hardware range, but signed metadata changed. */
    update_header_crc(package);
    mock_driver_init(&driver);
    assert(receive(package, size, &key, &driver, false) != 0);
    assert(driver.count_abort == 1U && driver.count_finalize == 0U && !driver.valid_flag);

    memcpy(package, original, size);
    package[HEADER_SIZE] ^= 1U;
    put_u32(package + 36U, ecat_foe_crc32_calc(package + HEADER_SIZE, size - HEADER_SIZE));
    update_header_crc(package);
    mock_driver_init(&driver);
    assert(receive(package, size, &key, &driver, false) != 0);
    assert(driver.count_abort == 1U && driver.count_finalize == 0U && !driver.valid_flag);

    mock_driver_init(&driver);
    assert(receive(original, size, &key, &driver, true) != 0);
    assert(driver.count_abort == 1U && driver.count_finalize == 0U && !driver.valid_flag);

    puts("PASS: CLI/Monocypher/core interop, unsigned, unknown key, tampering and corrupt readback");
    return 0;
}
