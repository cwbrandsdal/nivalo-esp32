// Codec conformance against nivalo-protocol fixtures/v1.1: messages.json (complete messages with
// the first failing rule) and the schema fixtures for every payload the device receives.

#include "NivaloDownlink.h"
#include "NivaloJsonStrict.h"
#include "NivaloOtaPolicy.h"
#include "NivaloWire.h"
#include "v11_test_support.h"

#include <cstring>
#include <set>
#include <vector>

using namespace v11test;

namespace
{
const char DeviceId[] = "22222222-2222-4222-8222-222222222222";

bool endsWith(const std::string &value, const std::string &suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string bytesOf(JsonObjectConst message)
{
    if (message["payload"].is<const char *>())
    {
        return str(message["payload"]);
    }
    const char *encoded = str(message["payloadBase64"]);
    std::vector<uint8_t> raw(std::strlen(encoded));
    size_t length = 0;
    V11_CHECK(nivaloBase64Decode(encoded, raw.data(), raw.size(), &length), "payloadBase64");
    return std::string(reinterpret_cast<const char *>(raw.data()), length);
}

int messageCases()
{
    std::unique_ptr<DynamicJsonDocument> document = loadFixture("fixtures/v1.1/messages.json");
    const std::set<std::string> scannerRules = {"encoding", "json", "duplicate-member", "depth", "value-count"};
    const std::set<std::string> unidentified = {"topic", "size", "encoding", "json", "duplicate-member", "depth",
                                                "value-count", "identity", "correlation", "command-seq"};
    NivaloTopics topics;
    topics.configure(DeviceId);
    int checked = 0;
    for (JsonObjectConst message : (*document)["cases"].as<JsonArrayConst>())
    {
        context() = std::string("messages.json ") + str(message["id"]) + ": " + str(message["description"]);
        const std::string topic = str(message["topic"]);
        const std::string bytes = bytesOf(message);
        const bool accept = std::strcmp(str(message["expect"]), "accept") == 0;
        const std::string rule = str(message["rule"]);

        // Route-independent JSON rules: the scanner every payload passes through.
        if (rule != "topic" && rule != "size")
        {
            const NivaloJsonRule scanned = nivaloScanJson(bytes.data(), bytes.size(), NivaloV11::MaxDepth,
                                                          NivaloV11::MaxValues, NULL);
            if (!accept && scannerRules.count(rule))
            {
                V11_CHECK(scanned != NIVALO_JSON_OK, "scanner accepted a " << rule << " violation");
            }
            else
            {
                V11_CHECK(scanned == NIVALO_JSON_OK, "scanner rejected (" << scanned << ")");
            }
        }

        // The device's receiver: only its own commands/{id} and ota/{id} routes.
        const bool downlink = !endsWith(topic, "/ack") &&
                              (topic.find("/commands/") != std::string::npos || topic.find("/ota/") != std::string::npos);
        NivaloOperation operation;
        const NivaloDownlinkOutcome outcome = nivaloParseDownlink(
            topics, topic.c_str(), reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), &operation);
        if (!downlink)
        {
            V11_CHECK(outcome == NIVALO_DOWNLINK_DROPPED, "an uplink route was taken as a downlink");
        }
        else if (accept)
        {
            V11_CHECK(outcome == NIVALO_DOWNLINK_IDENTIFIED && operation.valid, "valid downlink not accepted");
        }
        else if (unidentified.count(rule))
        {
            V11_CHECK(outcome == NIVALO_DOWNLINK_DROPPED, "a " << rule << " violation was identified");
        }
        else if (rule == "lifetime" || rule == "signature-encoding" || rule == "firmware-placement")
        {
            V11_CHECK(outcome == NIVALO_DOWNLINK_IDENTIFIED && !operation.valid,
                      "a " << rule << " violation must be identified and rejected as invalid_command");
        }
        else
        {
            V11_CHECK(!(outcome == NIVALO_DOWNLINK_IDENTIFIED && operation.valid), "a " << rule << " violation was accepted");
        }
        checked++;
    }
    return checked;
}

typedef bool (*Validator)(JsonVariantConst);

bool textValue(JsonVariantConst value) { return nivaloValidTextValue(value, (size_t)-1); }

struct SchemaBinding
{
    const char *file;
    const char *schemaSuffix;
    Validator validator;
};

int schemaCases()
{
    static const SchemaBinding Bindings[] = {
        {"command.json", "command.schema.json", nivaloValidCommandPayload},
        {"ota-command.json", "ota-command.schema.json", nivaloValidOtaPayload},
        {"ota-command.json", "ota-manifest.schema.json", nivaloValidOtaManifest},
        {"mqtt-envelope.json", "mqtt-envelope.schema.json", nivaloValidEnvelope},
        {"common.json", "#/$defs/uuid", nivaloValidUuidValue},
        {"common.json", "#/$defs/utc", nivaloValidUtcValue},
        {"common.json", "#/$defs/name", nivaloValidNameValue},
        {"common.json", "#/$defs/functionName", nivaloValidFunctionNameValue},
        {"common.json", "#/$defs/text", textValue},
        {"common.json", "#/$defs/positiveUint32", nivaloValidPositiveUint32},
        {"common.json", "#/$defs/firmware", nivaloValidFirmware},
    };
    int checked = 0;
    std::set<std::string> seen;
    for (const SchemaBinding &binding : Bindings)
    {
        std::unique_ptr<DynamicJsonDocument> document =
            loadFixture(std::string("fixtures/v1.1/schemas/") + binding.file);
        for (JsonObjectConst group : (*document)["groups"].as<JsonArrayConst>())
        {
            if (!endsWith(str(group["schema"]), binding.schemaSuffix))
            {
                continue;
            }
            seen.insert(binding.schemaSuffix);
            for (JsonObjectConst testCase : group["cases"].as<JsonArrayConst>())
            {
                context() = std::string(binding.file) + " " + binding.schemaSuffix + ": " + str(testCase["description"]);
                const bool expected = testCase["valid"].as<bool>();
                V11_CHECK(binding.validator(testCase["instance"]) == expected,
                          "expected " << (expected ? "valid" : "invalid"));
                checked++;
            }
        }
    }
    context() = "schema bindings";
    V11_CHECK(seen.size() == sizeof(Bindings) / sizeof(Bindings[0]), "a schema group was not found");
    return checked;
}

int primitiveCases()
{
    int checked = 0;
    context() = "utc round trip";
    int64_t ms = 0;
    char text[NivaloV11::UtcCapacity];
    V11_CHECK(nivaloParseUtc("2036-02-29T23:59:59.999Z", &ms) && nivaloFormatUtc(ms, text, sizeof(text)) &&
                  std::string(text) == "2036-02-29T23:59:59.999Z",
              text);
    V11_CHECK(nivaloParseUtc("2026-11-24T12:00:00Z", &ms) && ms == 1795521600000LL, ms);
    V11_CHECK(!nivaloFormatUtc(2145916800000LL, text, sizeof(text)), "2038 is outside the wire range");
    checked += 3;

    context() = "topics";
    NivaloTopics topics;
    V11_CHECK(topics.configure(DeviceId), "configure");
    V11_CHECK(std::string(topics.commandsFilter()) == std::string("nivalo/v1.1/devices/") + DeviceId + "/commands/+",
              topics.commandsFilter());
    V11_CHECK(std::string(topics.otaFilter()) == std::string("nivalo/v1.1/devices/") + DeviceId + "/ota/+", topics.otaFilter());
    char ack[128];
    V11_CHECK(topics.ack(NIVALO_OPERATION_OTA, "55555555-5555-4555-8555-55550000002b", ack, sizeof(ack)) &&
                  std::string(ack) == std::string("nivalo/v1.1/devices/") + DeviceId + "/ota/55555555-5555-4555-8555-55550000002b/ack",
              ack);
    V11_CHECK(!topics.configure("22222222-2222-4222-8222-22222222222A"), "uppercase device ID");
    checked += 5;

    context() = "seq binding";
    V11_CHECK(nivaloSeqMatchesId(42U, "44444444-4444-4444-8444-44440000002a"), "42");
    V11_CHECK(!nivaloSeqMatchesId(43U, "44444444-4444-4444-8444-44440000002a"), "43");
    V11_CHECK(nivaloSeqMatchesId(4294967295UL, "44444444-4444-4444-8444-4444ffffffff"), "max");
    checked += 3;
    return checked;
}
} // namespace

int main()
{
    const int messages = messageCases();
    const int schemas = schemaCases();
    const int primitives = primitiveCases();
    std::printf("messages %d, schema instances %d, primitives %d\n", messages, schemas, primitives);
    context() = "fixture coverage";
    V11_CHECK(messages >= 99 && schemas >= 158, "fixture sections were not found");
    const int checked = messages + schemas + primitives;
    return finish("v1.1 wire conformance", checked);
}
