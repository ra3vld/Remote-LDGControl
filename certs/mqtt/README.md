# MQTT TLS trust anchors

The `.der` files contain the public **ISRG Root X1 and ISRG Root X2** certificates
from Let's Encrypt. Sources:
[Root X1](https://letsencrypt.org/certs/isrgrootx1.pem) and
[Root X2](https://letsencrypt.org/certs/isrg-root-x2.pem).
Public certificate data is distributed under
[Mozilla Public License 2.0](https://www.mozilla.org/MPL/2.0/).

The compact bundle in `src/mqtt_ca_bundle.S` uses the ESP-IDF 5.5.5
little-endian offset-table format. It supplies the trust anchors for verified
MQTT TLS on port 8883. Brokers must present a valid chain to a trusted root and
a certificate matching the configured hostname.

To add a custom CA, place its public DER root certificate here and regenerate
with Python and the `cryptography` package:

```sh
python3 scripts/build_mqtt_ca.py
python3 scripts/build_mqtt_ca.py --check
```

Run these commands from the repository root, then rebuild the firmware.
Never add a private key or disable server verification.
