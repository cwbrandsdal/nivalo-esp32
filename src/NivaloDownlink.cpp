#include "NivaloDownlink.h"

#include <string.h>

#include <mbedtls/sha256.h>

#include "NivaloJsonStrict.h"
#include "NivaloOtaPolicy.h"

namespace
{
bool onlyMembers(JsonObjectConst object, const char *const *allowed, size_t count)
{
    for (JsonPairConst member : object)
    {
        bool found = false;
        for (size_t i = 0U; i < count && !found; i++)
        {
            found = strcmp(member.key().c_str(), allowed[i]) == 0;
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

bool has(JsonObjectConst object, const char *name)
{
    return object.containsKey(name);
}

const char *text(JsonVariantConst value)
{
    return value.is<const char *>() ? value.as<const char *>() : NULL;
}

bool integerIn(JsonVariantConst value, int64_t minimum, int64_t maximum)
{
    if (value.is<int64_t>())
    {
        const int64_t number = value.as<int64_t>();
        return number >= minimum && number <= maximum;
    }
    return false;
}

bool nonNull(JsonVariantConst value)
{
    if (value.isNull())
    {
        return false;
    }
    if (value.is<JsonObjectConst>())
    {
        for (JsonPairConst member : value.as<JsonObjectConst>())
        {
            if (!nonNull(member.value()))
            {
                return false;
            }
        }
    }
    else if (value.is<JsonArrayConst>())
    {
        for (JsonVariantConst item : value.as<JsonArrayConst>())
        {
            if (!nonNull(item))
            {
                return false;
            }
        }
    }
    return true;
}

bool matches(const char *value, size_t minimum, size_t maximum, bool (*allowed)(char))
{
    if (value == NULL)
    {
        return false;
    }
    const size_t length = strlen(value);
    if (length < minimum || length > maximum)
    {
        return false;
    }
    for (size_t i = 0U; i < length; i++)
    {
        if (!allowed(value[i]))
        {
            return false;
        }
    }
    return true;
}

bool keyIdChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

bool base64Char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

// ^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$
bool base64Pattern(const char *value)
{
    if (!matches(value, 12U, 96U, base64Char))
    {
        return false;
    }
    const size_t length = strlen(value);
    if (length % 4U != 0U)
    {
        return false;
    }
    for (size_t i = 0U; i < length; i++)
    {
        if (value[i] == '=')
        {
            const bool lastTwo = i >= length - 2U;
            if (!lastTwo || (i == length - 2U && value[length - 1U] != '='))
            {
                return false;
            }
        }
    }
    return true;
}

bool urlPathChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("._~!$&'()*+,;=:/?%-", c) != NULL;
}

// ^https://[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?(?::port)?/[A-Za-z0-9._~!$&'()*+,;=:/?%-]*$, <= 512 bytes
bool otaUrl(const char *value)
{
    if (value == NULL || strlen(value) > 512U)
    {
        return false;
    }
    NivaloOrigin origin;
    if (!nivaloParseHttpsOrigin(value, true, &origin))
    {
        return false;
    }
    const char *path = strchr(value + 8, '/');
    for (const char *cursor = path; cursor != NULL && *cursor != '\0'; cursor++)
    {
        if (!urlPathChar(*cursor))
        {
            return false;
        }
    }
    return path != NULL;
}

bool copyText(char *destination, size_t capacity, const char *source)
{
    if (source == NULL || strlen(source) >= capacity)
    {
        return false;
    }
    memcpy(destination, source, strlen(source) + 1U);
    return true;
}

bool lifetimeValid(int64_t issuedAtMs, int64_t expiresAtMs, int64_t maximumMs)
{
    const int64_t lifetime = expiresAtMs - issuedAtMs;
    return lifetime > 0 && lifetime <= maximumMs;
}
} // namespace

bool nivaloValidUuidValue(JsonVariantConst value)
{
    return nivaloIsUuid(text(value));
}

bool nivaloValidUtcValue(JsonVariantConst value)
{
    return nivaloParseUtc(text(value), NULL);
}

bool nivaloValidNameValue(JsonVariantConst value)
{
    return nivaloIsName(text(value));
}

bool nivaloValidFunctionNameValue(JsonVariantConst value)
{
    return nivaloIsFunctionName(text(value));
}

bool nivaloValidTextValue(JsonVariantConst value, size_t maxBytes)
{
    return nivaloIsText(text(value), maxBytes);
}

bool nivaloValidPositiveUint32(JsonVariantConst value)
{
    return integerIn(value, 1, 4294967295LL);
}

bool nivaloValidCommandArguments(JsonVariantConst arguments)
{
    return arguments.is<JsonObjectConst>() && nonNull(arguments);
}

bool nivaloValidFirmware(JsonVariantConst value)
{
    static const char *const Members[] = {"version", "hardware", "targets"};
    if (!value.is<JsonObjectConst>())
    {
        return false;
    }
    JsonObjectConst firmware = value.as<JsonObjectConst>();
    if (!onlyMembers(firmware, Members, 3U) || !has(firmware, "version") || !has(firmware, "hardware") ||
        !has(firmware, "targets") || !nivaloValidTextValue(firmware["version"], 64U) ||
        !nivaloValidTextValue(firmware["hardware"], 64U) || !firmware["targets"].is<JsonArrayConst>())
    {
        return false;
    }
    JsonArrayConst targets = firmware["targets"].as<JsonArrayConst>();
    if (targets.size() < 1U || targets.size() > 2U)
    {
        return false;
    }
    bool esp32 = false, stm32 = false;
    for (JsonVariantConst target : targets)
    {
        const char *name = text(target);
        if (name != NULL && strcmp(name, "esp32") == 0 && !esp32)
        {
            esp32 = true;
        }
        else if (name != NULL && strcmp(name, "stm32") == 0 && !stm32)
        {
            stm32 = true;
        }
        else
        {
            return false;
        }
    }
    return true;
}

bool nivaloValidEnvelope(JsonVariantConst value)
{
    static const char *const Members[] = {"schema", "messageId", "deviceId", "sentAt", "correlationId", "firmware", "payload"};
    if (!value.is<JsonObjectConst>())
    {
        return false;
    }
    JsonObjectConst envelope = value.as<JsonObjectConst>();
    const char *schema = text(envelope["schema"]);
    return onlyMembers(envelope, Members, 7U) && schema != NULL && strcmp(schema, NivaloV11::SchemaMarker) == 0 &&
           nivaloValidUuidValue(envelope["messageId"]) && nivaloValidUuidValue(envelope["deviceId"]) &&
           nivaloValidUtcValue(envelope["sentAt"]) &&
           (!has(envelope, "correlationId") || nivaloValidUuidValue(envelope["correlationId"])) &&
           (!has(envelope, "firmware") || nivaloValidFirmware(envelope["firmware"])) &&
           envelope["payload"].is<JsonObjectConst>();
}

bool nivaloValidCommandPayload(JsonVariantConst value)
{
    static const char *const Members[] = {"commandSeq", "issuedAt", "expiresAt", "command", "arguments", "requestedBy"};
    if (!value.is<JsonObjectConst>())
    {
        return false;
    }
    JsonObjectConst payload = value.as<JsonObjectConst>();
    if (!onlyMembers(payload, Members, 6U) || !nivaloValidPositiveUint32(payload["commandSeq"]) ||
        !nivaloValidUtcValue(payload["issuedAt"]) || !nivaloValidUtcValue(payload["expiresAt"]) ||
        !nivaloValidNameValue(payload["command"]) || !nivaloValidCommandArguments(payload["arguments"]) ||
        (has(payload, "requestedBy") && !nivaloValidTextValue(payload["requestedBy"], 128U)))
    {
        return false;
    }
    const char *command = text(payload["command"]);
    if (strcmp(command, "requestDefinitions") == 0)
    {
        return payload["arguments"].as<JsonObjectConst>().size() == 0U;
    }
    return !nivaloIsReservedFunctionName(command);
}

bool nivaloValidOtaManifest(JsonVariantConst value)
{
    static const char *const Members[] = {"format", "target", "productId", "securityVersion", "sizeBytes", "sha256"};
    if (!value.is<JsonObjectConst>())
    {
        return false;
    }
    JsonObjectConst manifest = value.as<JsonObjectConst>();
    const char *target = text(manifest["target"]);
    return onlyMembers(manifest, Members, 6U) && integerIn(manifest["format"], 2, 2) && target != NULL &&
           strcmp(target, "esp32") == 0 && nivaloValidUuidValue(manifest["productId"]) &&
           integerIn(manifest["securityVersion"], 1, 2147483647LL) && integerIn(manifest["sizeBytes"], 1, 134217728LL) &&
           nivaloIsLowerHex(text(manifest["sha256"]), 64U);
}

bool nivaloValidOtaPayload(JsonVariantConst value)
{
    static const char *const Members[] = {"commandSeq", "issuedAt", "expiresAt", "manifest", "url", "signature", "requestedBy"};
    static const char *const SignatureMembers[] = {"algorithm", "keyId", "value"};
    if (!value.is<JsonObjectConst>())
    {
        return false;
    }
    JsonObjectConst payload = value.as<JsonObjectConst>();
    if (!onlyMembers(payload, Members, 7U) || !nivaloValidPositiveUint32(payload["commandSeq"]) ||
        !nivaloValidUtcValue(payload["issuedAt"]) || !nivaloValidUtcValue(payload["expiresAt"]) ||
        !nivaloValidOtaManifest(payload["manifest"]) || !otaUrl(text(payload["url"])) ||
        !payload["signature"].is<JsonObjectConst>() ||
        (has(payload, "requestedBy") && !nivaloValidTextValue(payload["requestedBy"], 128U)))
    {
        return false;
    }
    JsonObjectConst signature = payload["signature"].as<JsonObjectConst>();
    const char *algorithm = text(signature["algorithm"]);
    return onlyMembers(signature, SignatureMembers, 3U) && algorithm != NULL &&
           strcmp(algorithm, "ecdsa-p256-sha256") == 0 && matches(text(signature["keyId"]), 1U, 64U, keyIdChar) &&
           base64Pattern(text(signature["value"]));
}

NivaloDownlinkOutcome nivaloParseDownlink(const NivaloTopics &topics, const char *topic, const uint8_t *payload,
                                          size_t length, NivaloOperation *operation)
{
    if (operation == NULL || payload == NULL)
    {
        return NIVALO_DOWNLINK_DROPPED;
    }
    *operation = NivaloOperation();
    NivaloOperationClass operationClass;
    char routeId[37];
    if (!topics.parseDownlink(topic, &operationClass, routeId))
    {
        return NIVALO_DOWNLINK_DROPPED;
    }
    const size_t limit = operationClass == NIVALO_OPERATION_OTA ? NivaloV11::MaxOtaBytes : NivaloV11::MaxCommandBytes;
    if (length > limit)
    {
        return NIVALO_DOWNLINK_DROPPED;
    }
    const char *bytes = reinterpret_cast<const char *>(payload);
    NivaloJsonScan scan;
    if (nivaloScanJson(bytes, length, NivaloV11::MaxDepth, NivaloV11::MaxValues, &scan) != NIVALO_JSON_OK ||
        !scan.rootIsObject || !scan.hasPayload)
    {
        return NIVALO_DOWNLINK_DROPPED;
    }
    // The fingerprint covers the payload value exactly as received, before anything is copied.
    if (mbedtls_sha256_ret(payload + scan.payloadOffset, scan.payloadLength, operation->fingerprint, 0) != 0)
    {
        return NIVALO_DOWNLINK_DROPPED;
    }

    // Read-only input: ArduinoJson copies strings instead of moving bytes inside the buffer.
    DynamicJsonDocument document(JSON_OBJECT_SIZE(NivaloV11::MaxValues) + length + 64U);
    if (deserializeJson(document, bytes, length) != DeserializationError::Ok)
    {
        return NIVALO_DOWNLINK_DROPPED;
    }
    JsonVariantConst envelope = document.as<JsonVariantConst>();
    if (!nivaloValidEnvelope(envelope))
    {
        return NIVALO_DOWNLINK_DROPPED;
    }
    const char *deviceId = text(envelope["deviceId"]);
    const char *correlationId = text(envelope["correlationId"]);
    if (strcmp(deviceId, topics.deviceId()) != 0 || correlationId == NULL || strcmp(correlationId, routeId) != 0)
    {
        return NIVALO_DOWNLINK_DROPPED;
    }
    JsonObjectConst body = envelope["payload"].as<JsonObjectConst>();
    if (!nivaloValidPositiveUint32(body["commandSeq"]))
    {
        return NIVALO_DOWNLINK_DROPPED;
    }
    const uint32_t seq = body["commandSeq"].as<uint32_t>();
    if (!nivaloSeqMatchesId(seq, routeId))
    {
        return NIVALO_DOWNLINK_DROPPED;
    }

    operation->operationClass = operationClass;
    memcpy(operation->id, routeId, sizeof(routeId));
    operation->seq = seq;

    if (operationClass == NIVALO_OPERATION_COMMAND)
    {
        if (!nivaloValidCommandPayload(body) || !nivaloParseUtc(text(body["issuedAt"]), &operation->issuedAtMs) ||
            !nivaloParseUtc(text(body["expiresAt"]), &operation->expiresAtMs) ||
            !lifetimeValid(operation->issuedAtMs, operation->expiresAtMs, NivaloV11::GenericLifetimeMs) ||
            !copyText(operation->command, sizeof(operation->command), text(body["command"])))
        {
            return NIVALO_DOWNLINK_IDENTIFIED;
        }
        const size_t written = serializeJson(body["arguments"], operation->arguments, sizeof(operation->arguments));
        operation->valid = written > 0U && written < sizeof(operation->arguments);
        return NIVALO_DOWNLINK_IDENTIFIED;
    }

    if (!nivaloValidOtaPayload(body) || !nivaloParseUtc(text(body["issuedAt"]), &operation->issuedAtMs) ||
        !nivaloParseUtc(text(body["expiresAt"]), &operation->expiresAtMs) ||
        !lifetimeValid(operation->issuedAtMs, operation->expiresAtMs, NivaloV11::OtaLifetimeMs) ||
        !nivaloStrictDerSignature(text(body["signature"]["value"])))
    {
        return NIVALO_DOWNLINK_IDENTIFIED;
    }
    JsonObjectConst manifest = body["manifest"].as<JsonObjectConst>();
    operation->valid = copyText(operation->manifest.productId, sizeof(operation->manifest.productId), text(manifest["productId"])) &&
                       copyText(operation->manifest.sha256, sizeof(operation->manifest.sha256), text(manifest["sha256"])) &&
                       copyText(operation->url, sizeof(operation->url), text(body["url"])) &&
                       copyText(operation->keyId, sizeof(operation->keyId), text(body["signature"]["keyId"])) &&
                       copyText(operation->signature, sizeof(operation->signature), text(body["signature"]["value"]));
    operation->manifest.securityVersion = manifest["securityVersion"].as<uint32_t>();
    operation->manifest.sizeBytes = manifest["sizeBytes"].as<uint32_t>();
    return NIVALO_DOWNLINK_IDENTIFIED;
}
