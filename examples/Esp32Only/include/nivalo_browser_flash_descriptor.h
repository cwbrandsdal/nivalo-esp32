#pragma once

#if !defined(NIVALO_BROWSER_FLASH_ARTIFACT) || NIVALO_BROWSER_FLASH_ARTIFACT != 1
#error "The browser-flash descriptor is reserved for the explicit provisioning artifact build."
#endif

// This fingerprint is independently recomputed from the exact 0x8000..0x8bff
// bytes by scripts/browser_flash_artifact.py. The post-build step fails closed
// if this declaration and the generated partition table differ.
#define NIVALO_BROWSER_FLASH_PARTITION_TABLE_SHA256 \
    "148b959cbff1c38aa8e1d5c0ba9d612c54997b945e56a63f41223eef650653a1"

__attribute__((used)) static const char nivaloBrowserFlashDescriptor[] =
    "NIVALO-BROWSER-FLASH-V1:{\"schema\":\"nivalo.browser-flash.v1\"," \
    "\"chipFamily\":\"ESP32\",\"chipVariant\":\"ESP32-D0WDQ6\"," \
    "\"boardId\":\"adafruit-feather-esp32\"," \
    "\"flashLayout\":\"nivalo-provisioning-4mb-v1\"," \
    "\"flashSizeBytes\":4194304,\"partitionTableSha256\":\"" \
    NIVALO_BROWSER_FLASH_PARTITION_TABLE_SHA256 "\"}";

__attribute__((used)) static const char nivaloBrowserFlashStoragePolicy[] =
    "NIVALO-PROVISIONING-STORAGE-POLICY-V1:evaluation-unencrypted-nvs";

__attribute__((used)) static const char nivaloBrowserFlashClaimEnvironment[] =
    "NIVALO-PROVISIONING-CLAIM-ENVIRONMENT-V1:staging";

inline void nivaloRetainBrowserFlashDescriptor()
{
    // `used` keeps compiler output; the reference keeps the section through
    // linker garbage collection. It has no runtime behavior or serial output.
    asm volatile("" : : "r"(nivaloBrowserFlashDescriptor) : "memory");
    asm volatile("" : : "r"(nivaloBrowserFlashStoragePolicy) : "memory");
    asm volatile("" : : "r"(nivaloBrowserFlashClaimEnvironment) : "memory");
}
