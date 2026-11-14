# OTA modem TLS trust

The HTTPS OTA path uses a public PEM trust anchor, `ThirdParty/ota_ca/gts-root-r4.pem`. PEM is a text encoding for an X.509 certificate. This certificate is public; it is not a password, token, or device identity. The firmware build verifies its fingerprint and generates an ignored C header from the PEM so the application can install it automatically into the SIMCom modem certificate store.

Before an HTTPS OTA request, the application:

1. Enables the modem's automatic network time and time-zone update (`AT+CTZU=1`).
2. Checks `AT+CCLK?` and stops if the modem has not received a plausible network date. It does not copy time from the MCU RTC.
3. Checks `AT+CCERTLIST`; if the named trust anchor is absent, uploads the embedded PEM with `AT+CCERTDOWN` and checks the list again.
4. Uses a dedicated SSL context with server verification enabled, SNI enabled, local-time checking enabled, and the configured CA file.

The OTA request fails closed if any of these steps fail. The firmware build confirms compilation and the CA generation script confirms the expected certificate fingerprint. Hardware validation is still required for the target A7670C firmware: network time arrival, certificate upload/list behavior, TLS handshake to the private R2 presigned URL, and successful/failed OTA recovery paths. Do not turn off CA verification or certificate-time validation to work around a failed handshake.

If the trust chain changes in future, validate the replacement CA against the current R2 endpoint and official certificate authority records, update the pinned fingerprint in `scripts/generate_ota_ca_header.py`, and bench-test before release.
