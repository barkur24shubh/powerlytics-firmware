#!/usr/bin/env python3
"""Generate the modem CA header from the reviewed public trust anchor."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PEM = ROOT / "ThirdParty/ota_ca/gts-root-r4.pem"
OUT = ROOT / "Core/Inc/ota_root_ca.h"
EXPECTED_SHA1 = "2A1D6027D94AB10A1C4D915CCD33A0CB3E2D54CB"
MAX_MODEM_CERT_BYTES = 10240


def main() -> None:
    pem = PEM.read_bytes()
    if not 0 < len(pem) <= MAX_MODEM_CERT_BYTES:
        raise SystemExit("OTA trust anchor size is outside SIMCom CCERTDOWN limits")

    result = subprocess.run(
        ["openssl", "x509", "-in", str(PEM), "-noout", "-fingerprint", "-sha1"],
        check=True,
        capture_output=True,
        text=True,
    )
    actual = result.stdout.strip().split("=", 1)[-1].replace(":", "").upper()
    if actual != EXPECTED_SHA1:
        raise SystemExit("OTA trust anchor fingerprint does not match the reviewed GTS Root R4")

    lines = pem.decode("ascii").splitlines(keepends=True)
    body = "\n".join(f'    "{line.rstrip(chr(10)).replace(chr(92), chr(92)*2).replace(chr(34), chr(92)+chr(34))}\\n"' for line in lines)
    OUT.write_text(
        "/* Generated from ThirdParty/ota_ca/gts-root-r4.pem; do not edit. */\n"
        "#ifndef OTA_ROOT_CA_H\n#define OTA_ROOT_CA_H\n\n"
        "static const unsigned char ota_root_ca_pem[] =\n"
        f"{body};\n"
        "#define OTA_ROOT_CA_PEM_SIZE ((unsigned int)(sizeof(ota_root_ca_pem) - 1U))\n\n"
        "#endif /* OTA_ROOT_CA_H */\n",
        encoding="ascii",
    )
    print(f"Generated {OUT.relative_to(ROOT)} ({len(pem)} certificate bytes; verified GTS Root R4 fingerprint)")


if __name__ == "__main__":
    main()
