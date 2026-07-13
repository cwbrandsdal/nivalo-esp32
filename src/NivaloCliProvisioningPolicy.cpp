#include "NivaloCliProvisioningPolicy.h"

#include <string.h>

namespace
{
bool isHex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool containsControl(const char *value, size_t length)
{
    if (value == NULL) return true;
    for (size_t index = 0U; index < length; ++index)
    {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (character < 0x20U || character == 0x7fU) return true;
    }
    return false;
}
} // namespace

namespace NivaloCliProvisioningPolicy
{
bool isRequestId(const char *value, size_t length)
{
    if (value == NULL || length != 32U) return false;
    for (size_t index = 0U; index < length; ++index)
        if (!isHex(value[index])) return false;
    return true;
}

bool isCanonicalUuid(const char *value, size_t length)
{
    if (value == NULL || length != 36U) return false;
    for (size_t index = 0U; index < length; ++index)
    {
        if (index == 8U || index == 13U || index == 18U || index == 23U)
        {
            if (value[index] != '-') return false;
        }
        else if (!isHex(value[index])) return false;
    }
    return true;
}

bool isWifiSsid(const char *value, size_t length)
{
    return length >= 1U && length <= 32U && !containsControl(value, length);
}

bool isWifiPassword(const char *value, size_t length)
{
    return (length == 0U || (length >= 8U && length <= 63U)) &&
           !containsControl(value, length);
}

bool isHardwareId(const char *value, size_t length)
{
    if (value == NULL || length != 18U || strncmp(value, "esp32-", 6U) != 0) return false;
    for (size_t index = 6U; index < length; ++index)
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) return false;
    return true;
}

bool isClaimCode(const char *value, size_t length)
{
    static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    if (value == NULL || length != 8U) return false;
    for (size_t index = 0U; index < length; ++index)
        if (strchr(alphabet, value[index]) == NULL) return false;
    return true;
}

bool isMqttHost(const char *value, size_t length)
{
    if (length < 1U || length > 253U || containsControl(value, length)) return false;
    size_t labelLength = 0U;
    for (size_t index = 0U; index < length; ++index)
    {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        const bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9');
        if (character == '.')
        {
            if (labelLength == 0U || value[index - 1U] == '-') return false;
            labelLength = 0U;
        }
        else
        {
            if ((!alphaNumeric && character != '-') || labelLength >= 63U ||
                (labelLength == 0U && character == '-')) return false;
            ++labelLength;
        }
    }
    return labelLength > 0U && value[length - 1U] != '-';
}

bool isMqttIdentity(const char *value, size_t length)
{
    return length >= 1U && length <= 128U && !containsControl(value, length);
}

bool isMqttPassword(const char *value, size_t length)
{
    return length >= 16U && length <= 512U && !containsControl(value, length);
}

bool isTlsPort(uint16_t port)
{
    return port == 8883U || port == 8884U;
}
} // namespace NivaloCliProvisioningPolicy
