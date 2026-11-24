// The image descriptor of this firmware build (specs/cloud-mqtt-v1.1.md sections 11.2-11.4).
//
// Set per environment at build time:
//   -DNIVALO_PRODUCT_ID=\"<product UUID>\"   staging and production use different product IDs
//   -DNIVALO_SECURITY_VERSION=<1..2147483647> raise it with every release
// Without a product ID every OTA job is rejected.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "NivaloOtaPolicy.h"

#ifndef NIVALO_PRODUCT_ID
#define NIVALO_PRODUCT_ID ""
#endif
#ifndef NIVALO_SECURITY_VERSION
#define NIVALO_SECURITY_VERSION 0
#endif

static_assert(sizeof(NIVALO_PRODUCT_ID) == 1U || sizeof(NIVALO_PRODUCT_ID) == 37U,
              "NIVALO_PRODUCT_ID must be empty or a canonical UUID");
static_assert((unsigned long long)(NIVALO_SECURITY_VERSION) <= 2147483647ULL,
              "NIVALO_SECURITY_VERSION must be at most 2147483647");

struct NivaloImageDescriptorLayout
{
    uint8_t magic[16];
    char productId[40];
    uint8_t securityVersion[4];
    uint8_t check[4];
};
static_assert(sizeof(NivaloImageDescriptorLayout) == NivaloImageDescriptorBytes, "descriptor layout");

namespace
{
#define NIVALO_IMAGE_MAGIC 0x4E, 0x56, 0x4C, 0x4F, 0xD3, 0x9A, 0x2B, 0xE1, 0x77, 0x05, 0xC6, 0x8F, 0x31, 0xB4, 0x6D, 0x10
constexpr uint8_t Magic[16] = {NIVALO_IMAGE_MAGIC};
constexpr uint32_t Version = (uint32_t)(NIVALO_SECURITY_VERSION);

constexpr uint32_t step(uint32_t hash, uint8_t byte) { return (uint32_t)((hash ^ byte) * 16777619ULL); }

constexpr uint8_t productByte(size_t index)
{
    return index < sizeof(NIVALO_PRODUCT_ID) - 1U ? (uint8_t)NIVALO_PRODUCT_ID[index] : (uint8_t)0U;
}

constexpr uint8_t descriptorByte(size_t index)
{
    return index < 16U ? Magic[index]
                       : index < 56U ? productByte(index - 16U) : (uint8_t)((Version >> (8U * (index - 56U))) & 0xFFU);
}

constexpr uint32_t checkOf(uint32_t hash, size_t index)
{
    return index == 60U ? hash : checkOf(step(hash, descriptorByte(index)), index + 1U);
}

constexpr uint32_t Check = checkOf(2166136261UL, 0U);
} // namespace

// External linkage and an escaping address keep exactly one copy of the descriptor in the image.
extern const NivaloImageDescriptorLayout nivaloImageDescriptor;
__attribute__((used)) const NivaloImageDescriptorLayout nivaloImageDescriptor = {
    {NIVALO_IMAGE_MAGIC},
    NIVALO_PRODUCT_ID,
    {(uint8_t)(Version & 0xFFU), (uint8_t)((Version >> 8U) & 0xFFU), (uint8_t)((Version >> 16U) & 0xFFU),
     (uint8_t)((Version >> 24U) & 0xFFU)},
    {(uint8_t)(Check & 0xFFU), (uint8_t)((Check >> 8U) & 0xFFU), (uint8_t)((Check >> 16U) & 0xFFU),
     (uint8_t)((Check >> 24U) & 0xFFU)}};

const uint8_t *nivaloRunningImageDescriptorBytes()
{
    return reinterpret_cast<const uint8_t *>(&nivaloImageDescriptor);
}

const NivaloImageIdentity &nivaloRunningImageIdentity()
{
    static NivaloImageIdentity identity;
    static bool loaded = false;
    if (!loaded)
    {
        // Read through the scanner so the running identity and a downloaded image's identity
        // are decoded by the same code.
        NivaloImageDescriptorScanner scanner;
        scanner.update(nivaloRunningImageDescriptorBytes(), NivaloImageDescriptorBytes);
        if (!scanner.identity(&identity))
        {
            identity = NivaloImageIdentity();
        }
        loaded = true;
    }
    return identity;
}
