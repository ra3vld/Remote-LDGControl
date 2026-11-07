MQTT TLS trust anchors: ISRG Root X1 and ISRG Root X2 (Let's Encrypt).
The tracked .der files contain only public root certificates (converted from PEM).
Sources: https://letsencrypt.org/certs/isrgrootx1.pem and https://letsencrypt.org/certs/isrg-root-x2.pem
Public certificate data is distributed under Mozilla Public License 2.0 (https://www.mozilla.org/MPL/2.0/).

The ESP32 full CA bundle exceeds the existing OTA partition. src/mqtt_ca_bundle.S
provides the default bundle symbols using these two roots. No partitions change.
Port 8883 requires a chain to these roots; custom/self-signed/other-CA brokers
will fail closed. To use another CA, add its public root here and regenerate with
the PlatformIO Python environment to run `scripts/build_mqtt_ca.py` (calls the pinned Espressif generator), keeping the same
sorted bundle format. Never disable verification or add a private key.
