#include "NivaloCliProvisioningPolicy.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
void require(bool condition, const char *message)
{
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}
} // namespace

int main()
{
    using namespace NivaloCliProvisioningPolicy;

    const std::string requestId(32U, 'a');
    require(isRequestId(requestId.c_str(), requestId.size()), "valid request ID rejected");
    require(!isRequestId("abc", 3U), "short request ID accepted");
    const std::string requestWithNewline = std::string(31U, 'a') + "\n";
    require(!isRequestId(requestWithNewline.c_str(), requestWithNewline.size()), "request ID control accepted");

    const std::string uuid = "00000000-0000-4000-8000-000000000000";
    require(isCanonicalUuid(uuid.c_str(), uuid.size()), "canonical UUID rejected");
    const std::string malformedUuid = "000000000000-4000-8000-000000000000";
    require(!isCanonicalUuid(malformedUuid.c_str(), malformedUuid.size()), "malformed UUID accepted");

    const std::string ssid = "bench-network";
    require(isWifiSsid(ssid.c_str(), ssid.size()), "valid SSID rejected");
    require(!isWifiSsid("", 0U), "empty SSID accepted");
    const std::string oversizedSsid(33U, 'x');
    require(!isWifiSsid(oversizedSsid.c_str(), oversizedSsid.size()), "oversized SSID accepted");

    require(isWifiPassword("", 0U), "open Wi-Fi password rejected");
    require(isWifiPassword("12345678", 8U), "valid Wi-Fi password rejected");
    require(!isWifiPassword("short", 5U), "short Wi-Fi password accepted");
    const std::string oversizedWifiPassword(64U, 'x');
    require(!isWifiPassword(oversizedWifiPassword.c_str(), oversizedWifiPassword.size()), "oversized Wi-Fi password accepted");

    require(isHardwareId("esp32-abcdef123456", 18U), "valid hardware ID rejected");
    require(!isHardwareId("esp32-ABCDEF123456", 18U), "noncanonical hardware ID accepted");
    require(!isHardwareId("esp32-abcdef12345", 17U), "short hardware ID accepted");
    require(isClaimCode("ABCD2345", 8U), "valid claim code rejected");
    require(!isClaimCode("ABCI2345", 8U), "ambiguous claim code accepted");
    require(!isClaimCode("ABCD234", 7U), "short claim code accepted");

    const std::string host = "mqtt-staging.nivalo.io";
    require(isMqttHost(host.c_str(), host.size()), "valid MQTT host rejected");
    require(!isMqttHost("mqtt/path", 9U), "MQTT host path accepted");
    require(!isMqttHost("mqtt host", 9U), "MQTT host whitespace accepted");
    require(!isMqttHost("-mqtt.example", 13U), "MQTT host leading hyphen accepted");
    require(!isMqttHost("mqtt..example", 13U), "MQTT host empty label accepted");
    require(!isMqttHost("mqtt.example.", 13U), "MQTT host trailing dot accepted");
    require(isTlsPort(8883U) && isTlsPort(8884U), "TLS broker port rejected");
    require(!isTlsPort(1883U), "plaintext broker port accepted");

    require(isMqttIdentity("device-client", 13U), "valid MQTT identity rejected");
    require(!isMqttIdentity("", 0U), "empty MQTT identity accepted");
    require(isMqttPassword("0123456789abcdef", 16U), "valid MQTT password rejected");
    require(!isMqttPassword("too-short", 9U), "short MQTT password accepted");
    const std::string secretWithNewline = "0123456789abcdef\n";
    require(!isMqttPassword(secretWithNewline.c_str(), secretWithNewline.size()), "MQTT password control accepted");

    require(MaximumLineLength == 16384U, "CLI NDJSON bound drifted");
    require(FrameTimeoutMs == 5000UL, "CLI frame timeout drifted");
    return 0;
}
