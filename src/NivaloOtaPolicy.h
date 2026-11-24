#ifndef NIVALO_OTA_POLICY_H
#define NIVALO_OTA_POLICY_H

// OTA decisions of specs/cloud-mqtt-v1.1.md section 11: manifest format 2 signature, device
// prechecks, image identity and boot reconciliation. Arduino-free; uses the mbedtls of the
// pinned ESP-IDF (2.28) on the device and the same mbedtls release on the host.

#include <stddef.h>
#include <stdint.h>

#include "NivaloDownlink.h"

struct NivaloFirmwareSigningKey
{
    const char *keyId;
    // PEM SubjectPublicKeyInfo of an ECDSA P-256 key. Public material only; the caller keeps
    // the storage valid for the device lifetime.
    const char *publicKeyPem;
};

// The device side of the precheck (section 11.3).
struct NivaloOtaDevice
{
    const NivaloFirmwareSigningKey *keys = NULL;
    size_t keyCount = 0U;
    // The product ID compiled into the running image; empty disables OTA.
    const char *productId = "";
    uint32_t runningSecurityVersion = 0U;
    uint32_t committedFloor = 0U;
    // Size of the inactive application slot.
    uint32_t maxImageBytes = 0U;
    // https://host[:port]; OTA downloads must come from this origin.
    const char *apiOrigin = NULL;
};

enum NivaloOtaPrecheck
{
    NIVALO_OTA_PRECHECK_OK = 0,
    NIVALO_OTA_PRECHECK_KEY,
    NIVALO_OTA_PRECHECK_SIGNATURE,
    NIVALO_OTA_PRECHECK_PRODUCT,
    NIVALO_OTA_PRECHECK_SECURITY_VERSION,
    NIVALO_OTA_PRECHECK_SIZE,
    NIVALO_OTA_PRECHECK_ORIGIN
};

// Prechecks in the spec's order, before any download.
NivaloOtaPrecheck nivaloOtaPrecheck(const NivaloOtaDevice &device, const NivaloOperation &operation);

// NIVALO-FIRMWARE-SIGNATURE-V2\n{target}\n{productId}\n{securityVersion}\n{sizeBytes}\n{sha256}\n
// Returns the length written, or 0 if the manifest is not canonical or the buffer is too small.
size_t nivaloCanonicalManifest(const NivaloOtaManifest &manifest, char *output, size_t capacity);

// ECDSA P-256/SHA-256 over exactly length bytes with a PEM public key; signature is Base64 DER.
bool nivaloVerifySignature(const char *publicKeyPem, const uint8_t *message, size_t length,
                           const char *signatureBase64);

// Canonical standard padded Base64. Returns false for any non-canonical input.
bool nivaloBase64Decode(const char *text, uint8_t *output, size_t capacity, size_t *length);

// signature-encoding rule (section 4.3): canonical Base64 of strict DER SEQUENCE { r, s } with
// minimal positive INTEGERs and 1 <= r, s < n (P-256 order).
bool nivaloStrictDerSignature(const char *base64);

struct NivaloOrigin
{
    char host[254] = {0};
    uint16_t port = 443U;
};

// Origin of an absolute HTTPS URL (lowercase host, optional port 1-65535). requirePath demands
// the "/path" part of a download URL; a configured origin may omit it.
bool nivaloParseHttpsOrigin(const char *url, bool requirePath, NivaloOrigin *origin);

// --- Image identity ----------------------------------------------------------------------------

// Every image embeds one descriptor with its product ID and security version. The downloaded
// image is scanned for it; the running image's own copy is nivaloRunningImageIdentity().
static const size_t NivaloImageDescriptorBytes = 64U;

struct NivaloImageIdentity
{
    char productId[37] = {0};
    uint32_t securityVersion = 0U;
};

// Identity of the running image (NIVALO_PRODUCT_ID and NIVALO_SECURITY_VERSION at build time).
const NivaloImageIdentity &nivaloRunningImageIdentity();
// The raw descriptor of the running image, for tests.
const uint8_t *nivaloRunningImageDescriptorBytes();

// Streaming search for valid descriptors in image bytes.
class NivaloImageDescriptorScanner
{
public:
    NivaloImageDescriptorScanner();
    void reset();
    void update(const uint8_t *data, size_t length);
    // Exactly one valid descriptor was found.
    bool identity(NivaloImageIdentity *identity) const;
    unsigned count() const { return _count; }

private:
    void candidate();
    uint8_t _magic[16];
    uint8_t _buffer[NivaloImageDescriptorBytes];
    size_t _matched = 0U;
    unsigned _count = 0U;
    bool _conflict = false;
    NivaloImageIdentity _identity;
};

// --- Persistent records ------------------------------------------------------------------------

// Pending OTA record (section 11.4 step 2), written from the signed manifest.
struct NivaloPendingRecord
{
    uint32_t commandSeq = 0U;
    char productId[37] = {0};
    uint32_t securityVersion = 0U;
    uint32_t sizeBytes = 0U;
    char sha256[65] = {0};
};

// Boot reconciliation (section 11.4 step 4): the running image equals the pending record when
// its embedded identity matches and the SHA-256 of the first sizeBytes bytes of its slot equals
// the signed digest. slotSha256Hex is that digest, or NULL when the slot is shorter.
bool nivaloImageMatchesPending(const NivaloImageIdentity &running, const char *slotSha256Hex,
                               const NivaloPendingRecord &pending);

void nivaloHexEncode(const uint8_t *bytes, size_t length, char *output);

#endif
