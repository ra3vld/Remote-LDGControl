#include "config.h"

#if TLS_ENABLED
#include "portal_cert.h"
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509.h>
#include <mbedtls/bignum.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecp.h>
#include <Preferences.h>
#include <SSLCert.hpp>

// Cert format generation. Bumped when we change algorithm so existing
// installs auto-regenerate the cert on next boot instead of needing a
// factory reset. Version 2 = ECDSA P-256 (hardware-accelerated on ESP32,
// ~10× faster TLS handshakes than the previous RSA-2048).
static const uint8_t PORTAL_CERT_VERSION = 2;

static String s_cert;
static String s_key;

static bool portalCert_generate() {
    Serial.println("Generating ECDSA P-256 key for portal HTTPS...");

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);

    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ctr_drbg_init(&ctr_drbg);

    unsigned char seed[32];
    esp_fill_random(seed, sizeof(seed));
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, seed, sizeof(seed));
    if (ret != 0) { Serial.printf("DRBG seed failed: -0x%04x\n", -ret); return false; }

    // EC key on secp256r1 (NIST P-256). ESP32 has hardware acceleration for
    // this curve via the RSA accelerator block — a fresh handshake completes
    // in ~200 ms vs ~2 s with software-only RSA-2048.
    ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret != 0) {
        Serial.printf("PK setup failed: -0x%04x\n", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }
    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                              mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        Serial.printf("EC keygen failed: -0x%04x\n", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }
    Serial.println("ECDSA P-256 key generated");

    Serial.println("Creating certificate...");

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);

    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    unsigned char serialBuf[16];
    mbedtls_ctr_drbg_random(&ctr_drbg, serialBuf, sizeof(serialBuf));
    mbedtls_mpi_read_binary(&serial, serialBuf, sizeof(serialBuf));
    mbedtls_x509write_crt_set_serial(&crt, &serial);
    mbedtls_mpi_free(&serial);

    mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000");
    mbedtls_x509write_crt_set_issuer_name(&crt, "CN=LDGConfig");
    mbedtls_x509write_crt_set_subject_name(&crt, "CN=LDGConfig");
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);

    unsigned char certBuf[1024];
    ret = mbedtls_x509write_crt_der(&crt, certBuf, sizeof(certBuf), mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret < 0) {
        Serial.printf("Cert write failed: -0x%04x\n", -ret);
        mbedtls_x509write_crt_free(&crt); mbedtls_pk_free(&pk);
        return false;
    }
    size_t certDerLen = ret;

    unsigned char keyBuf[1200];
    ret = mbedtls_pk_write_key_der(&pk, keyBuf, sizeof(keyBuf));
    if (ret < 0) {
        Serial.printf("Key write failed: -0x%04x\n", -ret);
        mbedtls_x509write_crt_free(&crt); mbedtls_pk_free(&pk);
        return false;
    }
    size_t keyDerLen = ret;

    // Store DER as base64 in NVS using mbedtls
    size_t b64certLen = 0, b64keyLen = 0;
    mbedtls_base64_encode(NULL, 0, &b64certLen, certBuf + sizeof(certBuf) - certDerLen, certDerLen);
    mbedtls_base64_encode(NULL, 0, &b64keyLen, keyBuf + sizeof(keyBuf) - keyDerLen, keyDerLen);

    unsigned char* b64cert = (unsigned char*)malloc(b64certLen);
    unsigned char* b64key = (unsigned char*)malloc(b64keyLen);
    mbedtls_base64_encode(b64cert, b64certLen, &b64certLen, certBuf + sizeof(certBuf) - certDerLen, certDerLen);
    mbedtls_base64_encode(b64key, b64keyLen, &b64keyLen, keyBuf + sizeof(keyBuf) - keyDerLen, keyDerLen);

    s_cert = String((const char*)b64cert);
    s_key = String((const char*)b64key);
    free(b64cert); free(b64key);

    Preferences certPrefs;
    certPrefs.begin("ldg-config", false);
    certPrefs.putString("portalCert", s_cert);
    certPrefs.putString("portalKey", s_key);
    certPrefs.putUChar("certVer", PORTAL_CERT_VERSION);
    certPrefs.end();

    mbedtls_x509write_crt_free(&crt); mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg); mbedtls_entropy_free(&entropy);

    Serial.println("Portal cert/key generated and stored in NVS");
    return true;
}

bool portalCert_init() {
    Preferences certPrefs;
    certPrefs.begin("ldg-config", false);

    String storedCert = certPrefs.getString("portalCert", "");
    String storedKey = certPrefs.getString("portalKey", "");
    uint8_t storedVer = certPrefs.getUChar("certVer", 1);  // pre-versioned RSA = v1

    // Force-regenerate if the on-disk cert is from an older format generation
    // (e.g. RSA-2048 from before the ECDSA switch). Cheaper than asking users
    // to factory-reset.
    if (storedVer < PORTAL_CERT_VERSION) {
        Serial.printf("portalCert: stored cert version %u < current %u, regenerating\n",
                      storedVer, PORTAL_CERT_VERSION);
        storedCert = ""; storedKey = "";
    }

    if (storedCert.length() > 0 && storedKey.length() > 0) {
        // Validate by decoding and parsing
        size_t keyDerLen = 0;
        mbedtls_base64_decode(NULL, 0, &keyDerLen, (const unsigned char*)storedKey.c_str(), storedKey.length());
        if (keyDerLen == 0) {
            Serial.println("portalCert: stored key decode failed");
            storedCert = ""; storedKey = "";
        } else {
            unsigned char* keyDer = (unsigned char*)malloc(keyDerLen);
            mbedtls_base64_decode(keyDer, keyDerLen, &keyDerLen, (const unsigned char*)storedKey.c_str(), storedKey.length());
            mbedtls_pk_context testPk;
            mbedtls_pk_init(&testPk);
            int ret = mbedtls_pk_parse_key(&testPk, keyDer, keyDerLen, NULL, 0);
            free(keyDer);
            mbedtls_pk_free(&testPk);
            if (ret != 0) {
                Serial.printf("portalCert: stored key invalid (ret=-0x%04x)\n", -ret);
                storedCert = ""; storedKey = "";
            }
        }
    }

    if (storedCert.length() == 0 || storedKey.length() == 0) {
        if (!portalCert_generate()) { certPrefs.end(); return false; }
        storedCert = certPrefs.getString("portalCert", "");
        storedKey = certPrefs.getString("portalKey", "");
    }

    s_cert = storedCert;
    s_key = storedKey;
    certPrefs.end();
    Serial.println("Portal cert/key loaded from NVS");
    return true;
}

const char* portalCert_getCert() { return s_cert.c_str(); }
const char* portalCert_getKey() { return s_key.c_str(); }

httpsserver::SSLCert* portalCert_newSSLCert() {
    if (s_cert.length() == 0 || s_key.length() == 0) {
        return nullptr;
    }

    size_t certDerLen = 0, keyDerLen = 0;
    mbedtls_base64_decode(nullptr, 0, &certDerLen,
                          (const unsigned char*)s_cert.c_str(), s_cert.length());
    mbedtls_base64_decode(nullptr, 0, &keyDerLen,
                          (const unsigned char*)s_key.c_str(), s_key.length());
    if (certDerLen == 0 || keyDerLen == 0) {
        return nullptr;
    }

    unsigned char* certDer = (unsigned char*)malloc(certDerLen);
    unsigned char* keyDer = (unsigned char*)malloc(keyDerLen);
    if (!certDer || !keyDer) {
        free(certDer);
        free(keyDer);
        return nullptr;
    }

    if (mbedtls_base64_decode(certDer, certDerLen, &certDerLen,
                              (const unsigned char*)s_cert.c_str(), s_cert.length()) != 0 ||
        mbedtls_base64_decode(keyDer, keyDerLen, &keyDerLen,
                              (const unsigned char*)s_key.c_str(), s_key.length()) != 0) {
        free(certDer);
        free(keyDer);
        return nullptr;
    }

    return new httpsserver::SSLCert(certDer, certDerLen, keyDer, keyDerLen);
}

#endif // TLS_ENABLED
