#ifndef PORTAL_CERT_H
#define PORTAL_CERT_H

#include <Arduino.h>

namespace httpsserver { class SSLCert; }

// Generate a unique self-signed certificate and ECDSA P-256 key when TLS is enabled.
// Stores them in NVS so they persist across reboots.
// This prevents the security issue of a hardcoded private key in firmware.

bool portalCert_init();
const char* portalCert_getCert();
const char* portalCert_getKey();

// Allocate a fresh SSLCert from the stored cert/key. The returned object
// owns its DER buffers — delete it to free everything. Callable any number
// of times; both the captive portal and the runtime HTTPS server use this.
// Returns nullptr if portalCert_init() has not produced a valid cert.
httpsserver::SSLCert* portalCert_newSSLCert();

#endif
