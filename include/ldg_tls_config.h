#pragma once

// Only the separately built TLS record library uses this configuration.
// Keep every ABI-affecting structure/feature flag intact. The Sunton MQTT
// profile negotiates RFC 6066 MFL=4096 (verified on the managed broker).
// Other profiles and optional web HTTPS keep the SDK's 16 KiB input record.
#ifdef LDG_TLS_SMALL_INPUT
#undef MBEDTLS_SSL_IN_CONTENT_LEN
#define MBEDTLS_SSL_IN_CONTENT_LEN 4096
#endif
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
