#ifndef NIVALO_CLI_PROVISIONING_POLICY_H
#define NIVALO_CLI_PROVISIONING_POLICY_H

#include <stddef.h>
#include <stdint.h>

namespace NivaloCliProvisioningPolicy
{
static const size_t MaximumLineLength = 16384U;
static const uint32_t FrameTimeoutMs = UINT32_C(5000);

bool isRequestId(const char *value, size_t length);
bool isCanonicalUuid(const char *value, size_t length);
bool isWifiSsid(const char *value, size_t length);
bool isWifiPassword(const char *value, size_t length);
bool isHardwareId(const char *value, size_t length);
bool isClaimCode(const char *value, size_t length);
// Per-unit setup code: 16 uppercase Crockford Base32 characters (80 bits), the WPA2 passphrase
// of the setup access point.
bool isSetupCode(const char *value, size_t length);
bool isMqttHost(const char *value, size_t length);
bool isMqttIdentity(const char *value, size_t length);
bool isMqttPassword(const char *value, size_t length);
bool isTlsPort(uint16_t port);
} // namespace NivaloCliProvisioningPolicy

#endif
