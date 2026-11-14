#include "monocypher-ed25519.h"
#include "ota_signature.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_decode(const char *text, uint8_t *out, size_t out_size)
{
    if (!text || !out || strlen(text) != out_size * 2U) return 0;
    for (size_t i = 0U; i < out_size; i++) {
        unsigned int value;
        if (sscanf(&text[i * 2U], "%2x", &value) != 1) return 0;
        out[i] = (uint8_t)value;
    }
    return 1;
}

int main(int argc, char **argv)
{
    uint8_t public_key[32];
    uint8_t signature[64];
    uint8_t sha256[32];
    char message[192];
    size_t message_len = 0U;
    char *end = NULL;
    unsigned long size;

    if (argc != 7) return 64;
    size = strtoul(argv[3], &end, 10);
    if (!end || *end != '\0' || size > UINT32_MAX ||
        !hex_decode(argv[4], sha256, sizeof(sha256)) ||
        !hex_decode(argv[5], public_key, sizeof(public_key)) ||
        !hex_decode(argv[6], signature, sizeof(signature)) ||
        !ota_signature_build_message(message, sizeof(message), &message_len,
                                     argv[1], argv[2], (uint32_t)size, sha256)) return 65;

    return crypto_ed25519_check(signature, public_key,
                                (const uint8_t *)message, message_len) == 0 ? 0 : 1;
}
