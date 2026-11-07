#if defined(ARDUINO_ARCH_ESP32) && defined(BOARD_ESP32_3248S035C)
#include "config.h"
#include <mbedtls/ssl.h>

extern "C" int __real_mbedtls_ssl_config_defaults(mbedtls_ssl_config*,int,int,int);
extern "C" int __wrap_mbedtls_ssl_config_defaults(mbedtls_ssl_config* config,
                                                 int endpoint,int transport,int preset) {
    const int result=__real_mbedtls_ssl_config_defaults(config,endpoint,transport,preset);
#if !TLS_ENABLED
    // Request the record limit before ClientHello. The broker must support
    // RFC 6066 MFL; a peer sending larger records fails closed, never insecure.
    if(result==0 && endpoint==MBEDTLS_SSL_IS_CLIENT)
        return mbedtls_ssl_conf_max_frag_len(config,MBEDTLS_SSL_MAX_FRAG_LEN_4096);
#endif
    return result;
}
#endif
