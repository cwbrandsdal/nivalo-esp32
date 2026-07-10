#ifndef NIVALO_FIRMWARE_SECURITY_H
#define NIVALO_FIRMWARE_SECURITY_H

#include <stddef.h>
#include <stdint.h>

#include "NivaloDevice.h"

// Builds the byte-for-byte manifest specified by cloud-mqtt-v1.md. Returns
// false unless target, size, and digest are already canonical.
bool nivaloBuildFirmwareManifest(
    const char *target,
    uint32_t sizeBytes,
    const char *sha256,
    char *manifest,
    size_t manifestCapacity,
    size_t *manifestLength);

// Verifies Base64-encoded ASN.1 DER ECDSA P-256/SHA-256 against a key selected
// by exact keyId. No fallback key and no unsigned mode exist.
bool nivaloVerifyFirmwareSignature(
    const NivaloFirmwareSigningKey *keys,
    size_t keyCount,
    const char *algorithm,
    const char *keyId,
    const char *signatureBase64,
    const char *manifest,
    size_t manifestLength,
    char *failure,
    size_t failureCapacity);

#endif
