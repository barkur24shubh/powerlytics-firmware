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
#define APP_OTA_STAGING_W25Q_ADDR     0x080000UL
#define APP_OTA_W25Q_TOTAL_SIZE       (2UL * 1024UL * 1024UL)

#define APP_OTA_MANIFEST_MAGIC        0x3141544FUL /* OTA1 */
#define APP_OTA_MANIFEST_VERSION      1UL
#define APP_OTA_STATE_PENDING         0x504E4447UL /* PNDG */
#define APP_OTA_RESULT_NONE           0xFFFFFFFFUL
#define APP_OTA_RESULT_INSTALLED      0x494E5354UL /* INST */
#define APP_OTA_RESULT_FAILED         0x4641494CUL /* FAIL */

#define APP_OTA_ERR_UNSET             0xFFFFFFFFUL
#define APP_OTA_ERR_NONE              0UL
#define APP_OTA_ERR_STAGED_HASH       1UL
#define APP_OTA_ERR_FLASH_PROGRAM     2UL
#define APP_OTA_ERR_FLASH_VERIFY      3UL

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
    char     deployment_id[64];
    char     target_version[32];
    uint32_t reserved[8];
    uint32_t crc32;
    uint32_t install_result;
    uint32_t install_error;
} app_ota_manifest_t;

#define APP_OTA_MANIFEST_CRC_LEN ((uint32_t)offsetof(app_ota_manifest_t, crc32))

#endif /* APP_OTA_MANIFEST_H */
