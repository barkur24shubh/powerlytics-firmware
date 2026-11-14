#ifndef OTA_SIGNATURE_H
#define OTA_SIGNATURE_H

#include <stddef.h>
#include <stdint.h>

uint8_t ota_signature_build_message(char *out,
                                   size_t capacity,
                                   size_t *out_len,
                                   const char *firmware_target,
                                   const char *version,
                                   uint32_t size_bytes,
                                   const uint8_t sha256[32]);

#endif
