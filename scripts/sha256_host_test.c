/* Host test for Core/Src/sha256.c (used by the app and the bootloader).
 * Build and run:  cc -I Core/Inc -o /tmp/sha256_test scripts/sha256_host_test.c Core/Src/sha256.c && /tmp/sha256_test
 * The 56-byte vector is the case the padding bug fixed on 2026-11-17 got wrong
 * (every input whose length mod 64 is 56..63). */
#include <stdio.h>
#include <string.h>
#include "sha256.h"

static int check(const char *name, const void *data, size_t len, const char *want)
{
    char got[65];
    sha256_hex((const uint8_t *)data, len, got);
    int ok = strcmp(got, want) == 0;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    return ok;
}

int main(void)
{
    int ok = 1;
    ok &= check("empty", "", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ok &= check("abc", "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    ok &= check("56 bytes (NIST 448-bit)", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    static char m[1000000];
    memset(m, 'a', sizeof(m));
    ok &= check("one million 'a'", m, sizeof(m), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    sha256_ctx_t c;
    char h[65];
    sha256_init(&c);
    for (size_t i = 0; i < sizeof(m); i += 997) sha256_update(&c, (const uint8_t *)m + i, (i + 997 <= sizeof(m)) ? 997 : sizeof(m) - i);
    sha256_final_hex(&c, h);
    int chunked = strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0;
    printf("%s one million 'a' in 997-byte updates\n", chunked ? "ok  " : "FAIL");
    return (ok && chunked) ? 0 : 1;
}
