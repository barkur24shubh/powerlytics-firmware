# Firmware Artifacts

Generated firmware files can contain embedded MQTT credentials and device
configuration strings. Do not commit artifacts automatically.

For a release or OTA build, generate fresh artifacts from the reviewed source and
publish only the intended `.bin`/`.hex`/`.elf` files with checksums.

## Layout

- `bootloader/`: first-stage image for `0x08000000`.
- `app/`: main gateway app linked for `0x08008000`; use this `.bin` for OTA.
- Root `BALCKPILL_BASED_GENERIC_GATEWAY.*`: compatibility copy of the current app artifact.
- `SHA256SUMS`: hashes for release verification and OTA metadata.
