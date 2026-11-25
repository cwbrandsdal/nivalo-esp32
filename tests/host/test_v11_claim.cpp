// The device-claim-v1 exchange response through the firmware's own validation and credential
// extraction (NivaloClaimResponse, as NivaloProvisioning::exchangeClaim() uses it). The response
// buffer is wiped before any value is read, as the firmware wipes and frees it straight after
// parsing: every extracted value must be owned by the parsed response, none may point into it.

#include "NivaloClaimResponse.h"
#include "v11_test_support.h"

#include <cstring>
#include <string>
#include <vector>

using namespace v11test;

namespace
{
const char DeviceId[] = "22222222-2222-4222-8222-222222222222";
const char Host[] = "mqtt.nivalo.io";
const char ClientId[] = "device-22222222-2222-4222-8222-222222222222";
const char Username[] = "nivalo-device-22222222";

std::string response(const std::string &ca, const std::string &extraMqtt = "", unsigned port = 8883U)
{
    std::string text = std::string("{\"deviceId\":\"") + DeviceId + "\",\"mqtt\":{\"host\":\"" + Host +
                       "\",\"port\":" + std::to_string(port) + ",\"useTls\":true,\"clientId\":\"" + ClientId +
                       "\",\"username\":\"" + Username + "\"";
    if (!ca.empty())
    {
        text += ",\"caCertificatePem\":" + ca;
    }
    return text + extraMqtt + "}}";
}

// A PEM body as a JSON string of exactly bodyBytes characters between the markers.
std::string pemString(size_t bodyBytes)
{
    std::string pem = "\"-----BEGIN CERTIFICATE-----\\n";
    pem += std::string(bodyBytes, 'A');
    return pem + "\\n-----END CERTIFICATE-----\\n\"";
}

// Parses text from a buffer that is wiped before the values are read.
bool parseThenWipe(NivaloClaimResponse &claim, const std::string &text)
{
    std::vector<char> buffer(NivaloClaimResponse::MaxBytes + 1U, '\0');
    if (text.size() > NivaloClaimResponse::MaxBytes)
    {
        buffer.resize(text.size() + 1U, '\0');
    }
    std::memcpy(buffer.data(), text.data(), text.size());
    const bool parsed = claim.parse(buffer.data(), text.size());
    std::memset(buffer.data(), 0, buffer.size());
    return parsed;
}
} // namespace

int main()
{
    int checked = 0;

    context() = "N1: a valid response yields its credentials after the buffer is wiped";
    {
        NivaloClaimResponse claim;
        const std::string text = response(pemString(64U));
        V11_CHECK(parseThenWipe(claim, text), "valid response refused");
        V11_CHECK(std::strcmp(claim.deviceId(), DeviceId) == 0, "deviceId '" << claim.deviceId() << "'");
        V11_CHECK(std::strcmp(claim.mqttHost(), Host) == 0, "host '" << claim.mqttHost() << "'");
        V11_CHECK(claim.mqttPort() == 8883U, "port " << claim.mqttPort());
        V11_CHECK(std::strcmp(claim.mqttClientId(), ClientId) == 0, "clientId '" << claim.mqttClientId() << "'");
        V11_CHECK(std::strcmp(claim.mqttUsername(), Username) == 0, "username '" << claim.mqttUsername() << "'");
        const std::string ca = std::string("-----BEGIN CERTIFICATE-----\n") + std::string(64U, 'A') +
                               "\n-----END CERTIFICATE-----\n";
        V11_CHECK(claim.mqttCaCertificate() == ca, "caCertificatePem '" << claim.mqttCaCertificate() << "'");
        checked++;
    }

    context() = "N1: a response at the 3072-byte bound with the largest CA certificate";
    {
        // caCertificatePem at 2048 bytes, the rest padded with whitespace to exactly 3072 bytes.
        const std::string markers = "-----BEGIN CERTIFICATE-----\n\n-----END CERTIFICATE-----\n";
        std::string text = response(pemString(2048U - markers.size()));
        V11_CHECK(text.size() <= NivaloClaimResponse::MaxBytes, "fixture too long: " << text.size());
        text.insert(1U, NivaloClaimResponse::MaxBytes - text.size(), ' ');
        NivaloClaimResponse claim;
        V11_CHECK(text.size() == NivaloClaimResponse::MaxBytes && parseThenWipe(claim, text), "bounded response refused");
        V11_CHECK(std::strlen(claim.mqttCaCertificate()) == 2048U && std::strcmp(claim.deviceId(), DeviceId) == 0 &&
                      std::strcmp(claim.mqttUsername(), Username) == 0,
                  "values lost at the bound");
        NivaloClaimResponse longer;
        V11_CHECK(!parseThenWipe(longer, " " + text), "a response over 3072 bytes was accepted");
        checked++;
    }

    context() = "a null or absent CA selects the built-in bundle";
    {
        NivaloClaimResponse claim;
        V11_CHECK(parseThenWipe(claim, response("null", "", 8884U)) && claim.mqttCaCertificate()[0] == '\0' &&
                      claim.mqttPort() == 8884U,
                  "null CA");
        V11_CHECK(parseThenWipe(claim, response("")) && claim.mqttCaCertificate()[0] == '\0', "absent CA");
        checked++;
    }

    context() = "responses outside the schema promote nothing";
    {
        const std::string refused[] = {
            response(pemString(64U), ",\"password\":\"x\""),
            response(pemString(64U), "", 1883U),
            response(pemString(2049U)),
            "{\"deviceId\":\"22222222-2222-4222-8222-22222222222A\",\"mqtt\":{}}",
            response(pemString(64U)) + "x",
            "[]",
        };
        for (const std::string &text : refused)
        {
            NivaloClaimResponse claim;
            V11_CHECK(!parseThenWipe(claim, text) && claim.deviceId()[0] == '\0', "accepted " << text.substr(0, 80));
        }
        checked++;
    }
    return finish("claim response", checked);
}
