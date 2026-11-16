#ifndef APP_OTA_MANIFEST_H
#define APP_OTA_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#define APP_OTA_BOOTLOADER_ADDR       0x08000000UL
#define APP_OTA_BOOTLOADER_SIZE       0x00008000UL

#define APP_OTA_APPLICATION_ADDR      0x08008000UL
#define APP_OTA_APPLICATION_SIZE      (512UL * 1024UL - APP_OTA_BOOTLOADER_SIZE)
#define APP_OTA_FLASH_END_ADDR        0x08080000UL

#define APP_OTA_MANIFEST_W25Q_ADDR    0x000000UL
#define APP_OTA_W25Q_TOTAL_SIZE       (2UL * 1024UL * 1024UL)
#define APP_OTA_SLOT_SIZE             APP_OTA_APPLICATION_SIZE
#define APP_OTA_BACKUP_W25Q_ADDR      0x080000UL
#define APP_OTA_STAGING_W25Q_ADDR     (APP_OTA_BACKUP_W25Q_ADDR + APP_OTA_SLOT_SIZE)

#define APP_OTA_MANIFEST_MAGIC        0x3141544FUL /* OTA1 */
#define APP_OTA_MANIFEST_VERSION      3UL
#define APP_OTA_STATE_PENDING         0x504E4447UL /* PNDG */
#define APP_OTA_RESULT_NONE           0xFFFFFFFFUL
#define APP_OTA_RESULT_INSTALLED      0x494E5354UL /* INST */
#define APP_OTA_RESULT_FAILED         0x4641494CUL /* FAIL */
/*
 * Rolled back. The result word lives on NOR flash and is only ever programmed
 * (bits 1 -> 0), never erased, so a later result must be a bit-subset of the
 * one it overwrites. Rollback always follows INSTALLED ("INST", 0x494E5354);
 * the old value "RBK1" (0x52424B31) programmed over it read back as
 * 0x40424310, which nothing recognised, so rollbacks were never reported.
 * 0x40424310 is exactly INST & RBK1: it can be written over INST cleanly, and
 * units already rolled back by an older bootloader hold this same value.
 */
#define APP_OTA_RESULT_ROLLED_BACK    0x40424310UL /* INST & "RBK1" */
#define APP_OTA_BACKUP_VALID          0x42555056UL /* BUPV */
#define APP_OTA_TRIAL_CONFIRMED       0x434F4E46UL /* CONF */
#define APP_OTA_ROLLBACK_REQUESTED    0x52424B51UL /* RBKQ */
#define APP_OTA_MAX_TRIAL_BOOTS       2UL

#define APP_OTA_ERR_UNSET             0xFFFFFFFFUL
#define APP_OTA_ERR_NONE              0UL
#define APP_OTA_ERR_STAGED_HASH       1UL
#define APP_OTA_ERR_FLASH_PROGRAM     2UL
#define APP_OTA_ERR_FLASH_VERIFY      3UL
#define APP_OTA_ERR_BACKUP            4UL
#define APP_OTA_ERR_TRIAL_EXHAUSTED   5UL
#define APP_OTA_ERR_MANUAL_ROLLBACK   6UL
#define APP_OTA_ERR_SIGNATURE_INVALID 7UL

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t state;
    uint32_t image_size;
    uint32_t image_addr;
    uint32_t app_addr;
    uint32_t app_max_size;
    uint8_t  sha256[32];
    uint8_t  rollback_sha256[32];
    uint8_t  signature_ed25519[64];
    char     deployment_id[64];
    char     target_version[32];
    char     rollback_version[32];
    uint32_t reserved[8];
    uint32_t crc32;
    uint32_t install_result;
    uint32_t install_error;
    uint32_t backup_valid;
    uint32_t trial_attempts;
    uint32_t trial_confirmed;
    uint32_t rollback_requested;
} app_ota_manifest_t;

#define APP_OTA_MANIFEST_CRC_LEN ((uint32_t)offsetof(app_ota_manifest_t, crc32))

#endif /* APP_OTA_MANIFEST_H */
