#include "NivaloClaimResponse.h"

#include "NivaloCliProvisioningPolicy.h"
#include "NivaloJsonStrict.h"
#include "NivaloWire.h"

#include <string.h>

namespace
{
// Room for every string of a MaxBytes response (copied, with terminators) and its few members.
const size_t DocumentCapacity = 4096U;

// clientId and username: the exact issued values, printable ASCII without space, 1-128 bytes.
bool issuedIdentity(JsonVariantConst value)
{
    if (!value.is<const char *>())
    {
        return false;
    }
    const char *text = value.as<const char *>();
    const size_t length = strlen(text);
    if (length < 1U || length > 128U)
    {
        return false;
    }
    for (size_t i = 0U; i < length; i++)
    {
        if (text[i] < '!' || text[i] > '~')
        {
            return false;
        }
    }
    return true;
}

bool validResponse(JsonVariantConst response)
{
    static const char *const MqttMembers[] = {"host", "port", "useTls", "clientId", "username", "caCertificatePem"};
    if (!response.is<JsonObjectConst>())
    {
        return false;
    }
    JsonObjectConst root = response.as<JsonObjectConst>();
    if (root.size() != 2U || !root.containsKey("deviceId") || !root["mqtt"].is<JsonObjectConst>() ||
        !root["deviceId"].is<const char *>() || !nivaloIsUuid(root["deviceId"].as<const char *>()))
    {
        return false;
    }
    JsonObjectConst mqtt = root["mqtt"].as<JsonObjectConst>();
    for (JsonPairConst member : mqtt)
    {
        bool known = false;
        for (const char *name : MqttMembers)
        {
            known = known || strcmp(member.key().c_str(), name) == 0;
        }
        if (!known)
        {
            return false;
        }
    }
    JsonVariantConst host = mqtt["host"];
    JsonVariantConst ca = mqtt["caCertificatePem"];
    return host.is<const char *>() &&
           NivaloCliProvisioningPolicy::isMqttHost(host.as<const char *>(), strlen(host.as<const char *>())) &&
           mqtt["port"].is<int>() && (mqtt["port"].as<int>() == 8883 || mqtt["port"].as<int>() == 8884) &&
           mqtt["useTls"].is<bool>() && mqtt["useTls"].as<bool>() && issuedIdentity(mqtt["clientId"]) &&
           issuedIdentity(mqtt["username"]) &&
           (!mqtt.containsKey("caCertificatePem") || ca.isNull() ||
            (ca.is<const char *>() && strlen(ca.as<const char *>()) <= 2048U));
}

const char *text(JsonVariantConst value)
{
    return value.is<const char *>() ? value.as<const char *>() : "";
}
} // namespace

NivaloClaimResponse::NivaloClaimResponse() : _document(DocumentCapacity) {}

bool NivaloClaimResponse::parse(const char *bytes, size_t length)
{
    _valid = false;
    _document.clear();
    if (bytes == NULL || length == 0U || length > MaxBytes || _document.capacity() == 0U ||
        nivaloScanJson(bytes, length, 4U, 32U, NULL) != NIVALO_JSON_OK)
    {
        return false;
    }
    // A const input: ArduinoJson copies every member name and string into the document. Given a
    // mutable buffer it would point into it instead, and the caller wipes and frees that buffer.
    if (deserializeJson(_document, bytes, length) != DeserializationError::Ok)
    {
        return false;
    }
    _valid = validResponse(_document.as<JsonVariantConst>());
    return _valid;
}

const char *NivaloClaimResponse::deviceId() const
{
    return _valid ? text(_document["deviceId"]) : "";
}

const char *NivaloClaimResponse::mqttHost() const
{
    return _valid ? text(_document["mqtt"]["host"]) : "";
}

uint16_t NivaloClaimResponse::mqttPort() const
{
    return _valid ? _document["mqtt"]["port"].as<uint16_t>() : 0U;
}

const char *NivaloClaimResponse::mqttClientId() const
{
    return _valid ? text(_document["mqtt"]["clientId"]) : "";
}

const char *NivaloClaimResponse::mqttUsername() const
{
    return _valid ? text(_document["mqtt"]["username"]) : "";
}

const char *NivaloClaimResponse::mqttCaCertificate() const
{
    return _valid ? text(_document["mqtt"]["caCertificatePem"]) : "";
}
