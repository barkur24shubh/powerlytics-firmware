#include "ota_signature.h"

#include <string.h>

static uint8_t append_bytes(char *out, size_t capacity, size_t *length,
                            const char *value, size_t value_len)
{
    if (!out || !length || !value || *length >= capacity || value_len >= capacity - *length) return 0U;
    memcpy(&out[*length], value, value_len);
    *length += value_len;
    out[*length] = '\0';
    return 1U;
}

static uint8_t append_u32(char *out, size_t capacity, size_t *length, uint32_t value)
{
    char digits[10];
    size_t count = 0U;
    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    if (*length + count >= capacity) return 0U;
    while (count > 0U) out[(*length)++] = digits[--count];
    out[*length] = '\0';
    return 1U;
}

static size_t bounded_length(const char *value, size_t max_len)
{
    size_t length = 0U;
    if (!value) return 0U;
    while (length < max_len && value[length] != '\0') length++;
    return length;
}

uint8_t ota_signature_build_message(char *out,
                                   size_t capacity,
                                   size_t *out_len,
                                   const char *firmware_target,
                                   const char *version,
                                   uint32_t size_bytes,
                                   const uint8_t sha256[32])
{
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "Powerlytics OTA v1\n";
    char sha_hex[64];
    size_t length = 0U;
    size_t target_len = bounded_length(firmware_target, 64U);
    size_t version_len = bounded_length(version, 32U);

    if (!out || capacity == 0U || !out_len || !sha256 || target_len == 0U || target_len >= 64U ||
        version_len == 0U || version_len >= 32U) return 0U;
    for (size_t i = 0U; i < target_len; i++) {
        if (firmware_target[i] == '\n' || firmware_target[i] == '\r') return 0U;
    }
    for (size_t i = 0U; i < version_len; i++) {
        if (version[i] == '\n' || version[i] == '\r') return 0U;
    }
    for (size_t i = 0U; i < 32U; i++) {
        sha_hex[i * 2U] = hex[sha256[i] >> 4U];
        sha_hex[i * 2U + 1U] = hex[sha256[i] & 0x0FU];
    }
    out[0] = '\0';
    if (!append_bytes(out, capacity, &length, prefix, sizeof(prefix) - 1U) ||
        !append_bytes(out, capacity, &length, firmware_target, target_len) ||
        !append_bytes(out, capacity, &length, "\n", 1U) ||
        !append_bytes(out, capacity, &length, version, version_len) ||
        !append_bytes(out, capacity, &length, "\n", 1U) ||
        !append_u32(out, capacity, &length, size_bytes) ||
        !append_bytes(out, capacity, &length, "\n", 1U) ||
        !append_bytes(out, capacity, &length, sha_hex, sizeof(sha_hex)) ||
        !append_bytes(out, capacity, &length, "\n", 1U)) return 0U;
    *out_len = length;
    return 1U;
}
