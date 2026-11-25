// Minimal mbedtls 2.28 configuration for the host tests: exactly the modules the firmware's
// protocol code uses on the device (SHA-256, PEM/DER public keys, ECDSA P-256 verification).
#ifndef NIVALO_MBEDTLS_HOST_CONFIG_H
#define NIVALO_MBEDTLS_HOST_CONFIG_H

#define MBEDTLS_HAVE_ASM
#define MBEDTLS_PLATFORM_C
// Verification only: no private-key operation needs the ECP blinding RNG.
#define MBEDTLS_ECP_NO_INTERNAL_RNG
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C

#include "mbedtls/check_config.h"

#endif
