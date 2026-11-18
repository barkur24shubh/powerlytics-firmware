#!/usr/bin/env python3
"""Generate Core/Inc/mqtt_root_ca.h from mqtt-broker-roots.pem.

The broker (EMQX Cloud) chains to DigiCert Global Root G2 (RSA). The bundle
also carries DigiCert Global Root CA and ISRG Root X1 so a CA change at the
broker does not take the fleet offline. Only public root certificates.
"""
import hashlib, pathlib
here = pathlib.Path(__file__).resolve().parent
pem = (here / "mqtt-broker-roots.pem").read_bytes().replace(b"\r\n", b"\n")
out = here.parent.parent / "Core" / "Inc" / "mqtt_root_ca.h"
lines = ["/* Generated from ThirdParty/mqtt_ca/mqtt-broker-roots.pem by generate_mqtt_ca_header.py; do not edit. */",
         "#ifndef MQTT_ROOT_CA_H", "#define MQTT_ROOT_CA_H", "",
         "/* sha256 of the bundle: %s */" % hashlib.sha256(pem).hexdigest(),
         "static const unsigned char mqtt_root_ca_pem[] ="]
for line in pem.decode().splitlines():
    lines.append('    "%s\\n"' % line)
lines[-1] += ";"
lines += ["#define MQTT_ROOT_CA_PEM_SIZE %uU" % len(pem), "", "#endif", ""]
out.write_text("\n".join(lines))
print(out, len(pem))
