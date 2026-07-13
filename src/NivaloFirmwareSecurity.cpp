#include "NivaloFirmwareSecurity.h"

#include <stdio.h>
#include <string.h>

#include <mbedtls/base64.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

static void setFailure(char *failure, size_t capacity, const char *message)
{
    if (failure == NULL || capacity == 0U)
    {
        return;
    }
    snprintf(failure, capacity, "%s", message == NULL ? "Firmware signature verification failed" : message);
}

static bool isCanonicalSha256(const char *value)
{
    if (value == NULL || strlen(value) != 64U)
    {
        return false;
    }
    for (size_t i = 0; i < 64U; i++)
    {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
        {
            return false;
        }
    }
    return true;
}

bool nivaloBuildFirmwareManifest(
    const char *target,
    uint32_t sizeBytes,
    const char *sha256,
    char *manifest,
    size_t manifestCapacity,
    size_t *manifestLength)
{
    if (manifest == NULL || manifestLength == NULL || sizeBytes == 0U ||
        (strcmp(target == NULL ? "" : target, "esp32") != 0 &&
         strcmp(target == NULL ? "" : target, "stm32") != 0) ||
        !isCanonicalSha256(sha256))
    {
        return false;
    }

    int written = snprintf(
        manifest,
        manifestCapacity,
        "NIVALO-FIRMWARE-SIGNATURE-V1\n%s\n%lu\n%s\n",
        target,
        (unsigned long)sizeBytes,
        sha256);
    if (written < 0 || (size_t)written >= manifestCapacity)
    {
        return false;
    }
    *manifestLength = (size_t)written;
    return true;
}

bool nivaloVerifyFirmwareSignature(
    const NivaloFirmwareSigningKey *keys,
    size_t keyCount,
    const char *algorithm,
    const char *keyId,
    const char *signatureBase64,
    const char *manifest,
    size_t manifestLength,
    char *failure,
    size_t failureCapacity)
{
    if (strcmp(algorithm == NULL ? "" : algorithm, "ecdsa-p256-sha256") != 0)
    {
        setFailure(failure, failureCapacity, "Unsupported firmware signature algorithm");
        return false;
    }
    if (keyId == NULL || keyId[0] == '\0' || signatureBase64 == NULL || signatureBase64[0] == '\0')
    {
        setFailure(failure, failureCapacity, "Firmware signature is missing");
        return false;
    }

    const char *publicKeyPem = NULL;
    for (size_t i = 0; keys != NULL && i < keyCount; i++)
    {
        if (keys[i].keyId != NULL && keys[i].publicKeyPem != NULL && strcmp(keys[i].keyId, keyId) == 0)
        {
            publicKeyPem = keys[i].publicKeyPem;
            break;
        }
    }
    if (publicKeyPem == NULL)
    {
        setFailure(failure, failureCapacity, "Firmware signature keyId is not trusted");
        return false;
    }

    uint8_t signatureDer[80];
    size_t signatureLength = 0U;
    int result = mbedtls_base64_decode(
        signatureDer,
        sizeof(signatureDer),
        &signatureLength,
        (const unsigned char *)signatureBase64,
        strlen(signatureBase64));
    if (result != 0 || signatureLength < 8U)
    {
        setFailure(failure, failureCapacity, "Firmware signature Base64/DER is malformed");
        return false;
    }

    mbedtls_pk_context publicKey;
    mbedtls_pk_init(&publicKey);
    result = mbedtls_pk_parse_public_key(
        &publicKey,
        (const unsigned char *)publicKeyPem,
        strlen(publicKeyPem) + 1U);
    if (result != 0 || !mbedtls_pk_can_do(&publicKey, MBEDTLS_PK_ECDSA))
    {
        mbedtls_pk_free(&publicKey);
        setFailure(failure, failureCapacity, "Trusted firmware public key is invalid");
        return false;
    }

    mbedtls_ecp_keypair *ec = mbedtls_pk_ec(publicKey);
    if (ec == NULL || ec->grp.id != MBEDTLS_ECP_DP_SECP256R1)
    {
        mbedtls_pk_free(&publicKey);
        setFailure(failure, failureCapacity, "Trusted firmware public key is not ECDSA P-256");
        return false;
    }

    uint8_t manifestHash[32];
    result = mbedtls_sha256_ret((const unsigned char *)manifest, manifestLength, manifestHash, 0);
    if (result == 0)
    {
        result = mbedtls_pk_verify(
            &publicKey,
            MBEDTLS_MD_SHA256,
            manifestHash,
            sizeof(manifestHash),
            signatureDer,
            signatureLength);
    }
    mbedtls_pk_free(&publicKey);

    if (result != 0)
    {
        setFailure(failure, failureCapacity, "Firmware manifest signature verification failed");
        return false;
    }
    return true;
}
