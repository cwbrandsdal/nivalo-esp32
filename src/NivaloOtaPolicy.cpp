#include "NivaloOtaPolicy.h"

#include <stdio.h>
#include <string.h>

#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

namespace
{
const char Base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char c)
{
    const char *found = c == '\0' ? NULL : strchr(Base64Alphabet, c);
    return found == NULL ? -1 : (int)(found - Base64Alphabet);
}

// P-256 group order n, big-endian.
const uint8_t P256Order[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51};

// 1 <= value < n for a big-endian unsigned integer without its sign byte.
bool inScalarRange(const uint8_t *value, size_t length)
{
    while (length > 0U && value[0] == 0U)
    {
        value++;
        length--;
    }
    if (length == 0U)
    {
        return false;
    }
    if (length != sizeof(P256Order))
    {
        return length < sizeof(P256Order);
    }
    return memcmp(value, P256Order, sizeof(P256Order)) < 0;
}

// The descriptor magic, masked so that the scanner itself never contains the plain bytes.
const volatile uint8_t MaskedMagic[16] = {0x14, 0x0C, 0x16, 0x15, 0x89, 0xC0, 0x71, 0xBB,
                                          0x2D, 0x5F, 0x9C, 0xD5, 0x6B, 0xEE, 0x37, 0x4A};
const uint8_t MagicMask = 0x5AU;

uint32_t fnv1a(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261UL;
    for (size_t i = 0U; i < length; i++)
    {
        hash ^= data[i];
        hash *= 16777619UL;
    }
    return hash;
}

uint32_t readLe32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

bool parsePort(const char *digits, size_t length, uint16_t *port)
{
    if (length == 0U || length > 5U || digits[0] == '0')
    {
        return false;
    }
    uint32_t value = 0U;
    for (size_t i = 0U; i < length; i++)
    {
        if (digits[i] < '0' || digits[i] > '9')
        {
            return false;
        }
        value = value * 10U + (uint32_t)(digits[i] - '0');
    }
    if (value < 1U || value > 65535U)
    {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}
} // namespace

void nivaloHexEncode(const uint8_t *bytes, size_t length, char *output)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0U; i < length; i++)
    {
        output[i * 2U] = hex[bytes[i] >> 4U];
        output[i * 2U + 1U] = hex[bytes[i] & 0x0FU];
    }
    output[length * 2U] = '\0';
}

bool nivaloBase64Decode(const char *text, uint8_t *output, size_t capacity, size_t *length)
{
    if (text == NULL || output == NULL || length == NULL)
    {
        return false;
    }
    const size_t textLength = strlen(text);
    if (textLength == 0U || textLength % 4U != 0U)
    {
        return false;
    }
    size_t written = 0U;
    for (size_t i = 0U; i < textLength; i += 4U)
    {
        const bool last = i + 4U == textLength;
        int values[4];
        size_t padding = 0U;
        for (size_t k = 0U; k < 4U; k++)
        {
            const char c = text[i + k];
            if (c == '=' && last && k >= 2U)
            {
                values[k] = 0;
                padding++;
                continue;
            }
            if (padding > 0U)
            {
                return false;
            }
            values[k] = base64Value(c);
            if (values[k] < 0)
            {
                return false;
            }
        }
        if (padding == 1U && text[i + 2U] == '=')
        {
            return false;
        }
        const uint32_t block = ((uint32_t)values[0] << 18U) | ((uint32_t)values[1] << 12U) |
                               ((uint32_t)values[2] << 6U) | (uint32_t)values[3];
        // Canonical: the bits dropped by padding must be zero.
        if ((padding == 1U && (block & 0xFFU) != 0U) || (padding == 2U && (block & 0xFFFFU) != 0U))
        {
            return false;
        }
        const size_t produced = 3U - padding;
        if (written + produced > capacity)
        {
            return false;
        }
        output[written++] = (uint8_t)(block >> 16U);
        if (produced > 1U) output[written++] = (uint8_t)(block >> 8U);
        if (produced > 2U) output[written++] = (uint8_t)block;
    }
    *length = written;
    return true;
}

bool nivaloStrictDerSignature(const char *base64)
{
    uint8_t raw[72];
    size_t length = 0U;
    if (!nivaloBase64Decode(base64, raw, sizeof(raw), &length) || length < 8U || length > 72U)
    {
        return false;
    }
    if (raw[0] != 0x30U || raw[1] != length - 2U)
    {
        return false;
    }
    size_t position = 2U;
    for (int part = 0; part < 2; part++)
    {
        if (position + 2U > length || raw[position] != 0x02U)
        {
            return false;
        }
        const size_t integerLength = raw[position + 1U];
        const uint8_t *body = raw + position + 2U;
        if (integerLength == 0U || position + 2U + integerLength > length || (body[0] & 0x80U) != 0U)
        {
            return false;
        }
        if (integerLength > 1U && body[0] == 0U && (body[1] & 0x80U) == 0U)
        {
            return false;
        }
        if (!inScalarRange(body, integerLength))
        {
            return false;
        }
        position += 2U + integerLength;
    }
    return position == length;
}

size_t nivaloCanonicalManifest(const NivaloOtaManifest &manifest, char *output, size_t capacity)
{
    if (output == NULL || !nivaloIsUuid(manifest.productId) || !nivaloIsLowerHex(manifest.sha256, 64U) ||
        manifest.securityVersion == 0U || manifest.sizeBytes == 0U)
    {
        return 0U;
    }
    const int written = snprintf(output, capacity, "NIVALO-FIRMWARE-SIGNATURE-V2\nesp32\n%s\n%lu\n%lu\n%s\n",
                                 manifest.productId, (unsigned long)manifest.securityVersion,
                                 (unsigned long)manifest.sizeBytes, manifest.sha256);
    if (written <= 0 || (size_t)written >= capacity)
    {
        return 0U;
    }
    return (size_t)written;
}

bool nivaloVerifySignature(const char *publicKeyPem, const uint8_t *message, size_t length,
                           const char *signatureBase64)
{
    if (publicKeyPem == NULL || message == NULL || !nivaloStrictDerSignature(signatureBase64))
    {
        return false;
    }
    uint8_t signature[72];
    size_t signatureLength = 0U;
    if (!nivaloBase64Decode(signatureBase64, signature, sizeof(signature), &signatureLength))
    {
        return false;
    }
    uint8_t digest[32];
    if (mbedtls_sha256_ret(message, length, digest, 0) != 0)
    {
        return false;
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool verified = false;
    if (mbedtls_pk_parse_public_key(&key, reinterpret_cast<const unsigned char *>(publicKeyPem),
                                    strlen(publicKeyPem) + 1U) == 0 &&
        mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA))
    {
        const mbedtls_ecp_keypair *ec = mbedtls_pk_ec(key);
        verified = ec != NULL && ec->grp.id == MBEDTLS_ECP_DP_SECP256R1 &&
                   mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), signature,
                                     signatureLength) == 0;
    }
    mbedtls_pk_free(&key);
    return verified;
}

bool nivaloParseHttpsOrigin(const char *url, bool requirePath, NivaloOrigin *origin)
{
    static const char Scheme[] = "https://";
    if (url == NULL || origin == NULL || strncmp(url, Scheme, sizeof(Scheme) - 1U) != 0)
    {
        return false;
    }
    const char *host = url + sizeof(Scheme) - 1U;
    size_t hostLength = 0U;
    while (host[hostLength] != '\0' && host[hostLength] != ':' && host[hostLength] != '/')
    {
        const char c = host[hostLength];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
        {
            return false;
        }
        hostLength++;
    }
    if (hostLength == 0U || hostLength > 253U || host[0] == '.' || host[0] == '-' ||
        host[hostLength - 1U] == '.' || host[hostLength - 1U] == '-')
    {
        return false;
    }
    const char *rest = host + hostLength;
    uint16_t port = 443U;
    if (*rest == ':')
    {
        const char *digits = rest + 1;
        const size_t digitCount = strcspn(digits, "/");
        if (!parsePort(digits, digitCount, &port))
        {
            return false;
        }
        rest = digits + digitCount;
    }
    if (*rest == '\0')
    {
        if (requirePath)
        {
            return false;
        }
    }
    else if (*rest != '/' || (!requirePath && rest[1] != '\0'))
    {
        return false;
    }
    memcpy(origin->host, host, hostLength);
    origin->host[hostLength] = '\0';
    origin->port = port;
    return true;
}

NivaloOtaPrecheck nivaloOtaPrecheck(const NivaloOtaDevice &device, const NivaloOperation &operation)
{
    const NivaloFirmwareSigningKey *key = NULL;
    for (size_t i = 0U; device.keys != NULL && i < device.keyCount; i++)
    {
        if (device.keys[i].keyId != NULL && strcmp(device.keys[i].keyId, operation.keyId) == 0)
        {
            key = &device.keys[i];
            break;
        }
    }
    if (key == NULL || key->publicKeyPem == NULL)
    {
        return NIVALO_OTA_PRECHECK_KEY;
    }
    char canonical[192];
    const size_t canonicalLength = nivaloCanonicalManifest(operation.manifest, canonical, sizeof(canonical));
    if (canonicalLength == 0U ||
        !nivaloVerifySignature(key->publicKeyPem, reinterpret_cast<const uint8_t *>(canonical), canonicalLength,
                               operation.signature))
    {
        return NIVALO_OTA_PRECHECK_SIGNATURE;
    }
    if (device.productId == NULL || !nivaloIsUuid(device.productId) ||
        strcmp(device.productId, operation.manifest.productId) != 0)
    {
        return NIVALO_OTA_PRECHECK_PRODUCT;
    }
    const uint32_t floor =
        device.runningSecurityVersion > device.committedFloor ? device.runningSecurityVersion : device.committedFloor;
    if (!device.floorReadable || operation.manifest.securityVersion <= floor)
    {
        return NIVALO_OTA_PRECHECK_SECURITY_VERSION;
    }
    if (operation.manifest.sizeBytes > device.maxImageBytes)
    {
        return NIVALO_OTA_PRECHECK_SIZE;
    }
    NivaloOrigin download, configured;
    if (!nivaloParseHttpsOrigin(operation.url, true, &download) ||
        !nivaloParseHttpsOrigin(device.apiOrigin, false, &configured) || download.port != configured.port ||
        strcmp(download.host, configured.host) != 0)
    {
        return NIVALO_OTA_PRECHECK_ORIGIN;
    }
    return NIVALO_OTA_PRECHECK_OK;
}

// --- Image identity ----------------------------------------------------------------------------

NivaloImageDescriptorScanner::NivaloImageDescriptorScanner()
{
    reset();
}

void NivaloImageDescriptorScanner::reset()
{
    for (size_t i = 0U; i < sizeof(_magic); i++)
    {
        _magic[i] = (uint8_t)(MaskedMagic[i] ^ MagicMask);
    }
    _collected = 0U;
    _matched = 0U;
    _magicCount = 0U;
    _count = 0U;
    _conflict = false;
    _identity = NivaloImageIdentity();
}

void NivaloImageDescriptorScanner::update(const uint8_t *data, size_t length)
{
    for (size_t i = 0U; data != NULL && i < length; i++)
    {
        const uint8_t byte = data[i];
        // Every occurrence of the magic is counted, also one that starts inside a candidate. The
        // magic's bytes are pairwise distinct, so a mismatch can only restart a match at byte 0.
        bool magicEnds = false;
        if (byte == _magic[_matched])
        {
            _matched++;
            if (_matched == sizeof(_magic))
            {
                _magicCount++;
                _matched = 0U;
                magicEnds = true;
            }
        }
        else
        {
            _matched = byte == _magic[0] ? 1U : 0U;
        }

        if (_collected > 0U)
        {
            _buffer[_collected++] = byte;
            if (_collected == NivaloImageDescriptorBytes)
            {
                candidate();
                _collected = 0U;
            }
        }
        else if (magicEnds)
        {
            // A candidate starts with the magic that just ended.
            memcpy(_buffer, _magic, sizeof(_magic));
            _collected = sizeof(_magic);
        }
    }
}

void NivaloImageDescriptorScanner::candidate()
{
    if (readLe32(_buffer + 60) != fnv1a(_buffer, 60U))
    {
        return;
    }
    NivaloImageIdentity found;
    const char *product = reinterpret_cast<const char *>(_buffer + 16);
    const size_t productLength = strnlen(product, 40U);
    if (productLength == 36U)
    {
        memcpy(found.productId, product, 36U);
    }
    else if (productLength != 0U)
    {
        return;
    }
    for (size_t i = productLength; i < 40U; i++)
    {
        if (product[i] != '\0')
        {
            return;
        }
    }
    found.securityVersion = readLe32(_buffer + 56);
    if (_count > 0U && (strcmp(found.productId, _identity.productId) != 0 ||
                        found.securityVersion != _identity.securityVersion))
    {
        _conflict = true;
    }
    _identity = found;
    _count++;
}

bool NivaloImageDescriptorScanner::identity(NivaloImageIdentity *identity) const
{
    if (_count != 1U || _magicCount != 1U || _conflict)
    {
        return false;
    }
    if (identity != NULL)
    {
        *identity = _identity;
    }
    return true;
}

bool nivaloImageMatchesPending(const NivaloImageIdentity &running, const char *slotSha256Hex,
                               const NivaloPendingRecord &pending)
{
    return slotSha256Hex != NULL && strcmp(running.productId, pending.productId) == 0 &&
           running.securityVersion == pending.securityVersion && strcmp(slotSha256Hex, pending.sha256) == 0;
}
