# Firmware Artifacts

Generated firmware files can contain embedded MQTT credentials and device
configuration strings. Do not commit artifacts automatically.

For a release or OTA build, generate fresh artifacts from the reviewed source and
publish only the intended `.bin`/`.hex`/`.elf` files with checksums.
