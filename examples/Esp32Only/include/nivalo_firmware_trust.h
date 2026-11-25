#pragma once

// Firmware signing trust anchors, one per environment (specs/cloud-mqtt-v1.1.md section 11.2).
// Public keys only: the matching private keys stay in the platform's signing service
// (Nivalo.<env>.Firmware.ArtifactSigning.CurrentPrivateKeyPkcs8Base64 in SecretStore) and never
// appear in firmware, this repository or a device. The environment is chosen by platformio.ini
// (NIVALO_ENVIRONMENT_STAGING or NIVALO_ENVIRONMENT_PRODUCTION); main.cpp refuses to build a
// staging or production image without its anchor or with a reserved test- key ID.
//
// Rotation: add the new key as CURRENT and move the old one to PREVIOUS (both here, inside the
// environment's block), ship that firmware, sign with the new key once it is established, then
// drop PREVIOUS (README, "Firmware updates").

#if defined(NIVALO_ENVIRONMENT_PRODUCTION) && defined(NIVALO_ENVIRONMENT_STAGING)
#error "Select one environment: NIVALO_ENVIRONMENT_PRODUCTION or NIVALO_ENVIRONMENT_STAGING"
#endif

// Both slots, current and previous, come from this tracked file in a staging or production build:
// a key slot defined anywhere else (nivalo_config.h, a build flag) fails the build.
#if (defined(NIVALO_ENVIRONMENT_PRODUCTION) || defined(NIVALO_ENVIRONMENT_STAGING)) &&             \
    (defined(NIVALO_FIRMWARE_CURRENT_KEY_ID) || defined(NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM) || \
     defined(NIVALO_FIRMWARE_PREVIOUS_KEY_ID) || defined(NIVALO_FIRMWARE_PREVIOUS_PUBLIC_KEY_PEM))
#error "Staging and production trust anchors (current and previous) come from nivalo_firmware_trust.h only; remove the local key definitions"
#endif

#if defined(NIVALO_ENVIRONMENT_PRODUCTION)
#define NIVALO_FIRMWARE_CURRENT_KEY_ID "nivalo-prod-2026-11"
#define NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM \
    "-----BEGIN PUBLIC KEY-----\n" \
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEJa8w85D4JY5/RpfpCnQ1b4iycuCX\n" \
    "2QsO1dc/6fFkqjRoBWu2Ov0Pz8y7PFASmj/RXMJ2UoSSHqy2pw0eOEryfQ==\n" \
    "-----END PUBLIC KEY-----\n"
#elif defined(NIVALO_ENVIRONMENT_STAGING)
#define NIVALO_FIRMWARE_CURRENT_KEY_ID "staging-d364850176bd64cd"
#define NIVALO_FIRMWARE_CURRENT_PUBLIC_KEY_PEM \
    "-----BEGIN PUBLIC KEY-----\n" \
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEKoEORGU4tW+CdPe+1kD7IPKXhP7w\n" \
    "1jZHWx0FmmZJHyaOdYPksWxu26oMhDLCcAPchy7sSrkDQ14vluz1b4FcTQ==\n" \
    "-----END PUBLIC KEY-----\n"
#endif
