// OTA decisions against nivalo-protocol fixtures/v1.1/vectors/ota.json: canonical manifest bytes,
// ECDSA verification through mbedtls, signature mutations and the device prechecks of section
// 11.3; plus the image descriptor and the persistent record encodings the install path relies on.

#include "NivaloDownlink.h"
#include "NivaloJournal.h"
#include "NivaloOtaPolicy.h"
#include "v11_test_support.h"

#include <cstring>
#include <map>
#include <vector>

using namespace v11test;

namespace
{
const char DeviceId[] = "22222222-2222-4222-8222-222222222222";

struct Device
{
    std::vector<std::string> keyIds, pems;
    std::vector<NivaloFirmwareSigningKey> keys;
    std::string productId, apiOrigin;
    NivaloOtaDevice ota;

    Device(JsonObjectConst base, JsonObjectConst overrides)
    {
        JsonObjectConst trusted = overrides.containsKey("trustedKeys") ? overrides["trustedKeys"].as<JsonObjectConst>()
                                                                        : base["trustedKeys"].as<JsonObjectConst>();
        for (JsonPairConst key : trusted)
        {
            keyIds.push_back(key.key().c_str());
            pems.push_back(pemFromSpkiBase64(str(key.value())));
        }
        for (size_t i = 0; i < pems.size(); i++)
        {
            NivaloFirmwareSigningKey key = {keyIds[i].c_str(), pems[i].c_str()};
            keys.push_back(key);
        }
        productId = str(overrides.containsKey("productId") ? overrides["productId"] : base["productId"]);
        apiOrigin = str(overrides.containsKey("apiOrigin") ? overrides["apiOrigin"] : base["apiOrigin"]);
        ota.keys = keys.data();
        ota.keyCount = keys.size();
        ota.productId = productId.c_str();
        ota.apiOrigin = apiOrigin.c_str();
        ota.runningSecurityVersion = (overrides.containsKey("compiledSecurityVersion") ? overrides : base)["compiledSecurityVersion"].as<uint32_t>();
        ota.committedFloor = (overrides.containsKey("committedFloor") ? overrides : base)["committedFloor"].as<uint32_t>();
        ota.maxImageBytes = (overrides.containsKey("maxImageBytes") ? overrides : base)["maxImageBytes"].as<uint32_t>();
    }
};

bool parseOta(JsonVariantConst payload, NivaloOperation *operation)
{
    char id[37];
    std::snprintf(id, sizeof(id), "55555555-5555-4555-8555-5555%08lx", payload["commandSeq"].as<unsigned long>());
    const std::string envelope = std::string("{\"schema\":\"nivalo.iot.v1.1\",\"messageId\":\"33333333-3333-4333-8333-3333000000ef\",") +
                                 "\"deviceId\":\"" + DeviceId + "\",\"sentAt\":\"2026-11-24T12:00:05Z\",\"correlationId\":\"" + id +
                                 "\",\"payload\":" + compact(payload) + "}";
    const std::string topic = std::string("nivalo/v1.1/devices/") + DeviceId + "/ota/" + id;
    NivaloTopics topics;
    topics.configure(DeviceId);
    return nivaloParseDownlink(topics, topic.c_str(), reinterpret_cast<const uint8_t *>(envelope.data()), envelope.size(),
                               operation) == NIVALO_DOWNLINK_IDENTIFIED &&
           operation->valid;
}

NivaloOtaManifest manifestFrom(JsonVariantConst value)
{
    NivaloOtaManifest manifest;
    std::strncpy(manifest.productId, str(value["productId"]), 36U);
    manifest.securityVersion = value["securityVersion"].as<uint32_t>();
    manifest.sizeBytes = value["sizeBytes"].as<uint32_t>();
    std::strncpy(manifest.sha256, str(value["sha256"]), 64U);
    return manifest;
}

const char *precheckName(NivaloOtaPrecheck result)
{
    switch (result)
    {
    case NIVALO_OTA_PRECHECK_OK: return "download";
    case NIVALO_OTA_PRECHECK_KEY: return "key";
    case NIVALO_OTA_PRECHECK_SIGNATURE: return "signature";
    case NIVALO_OTA_PRECHECK_PRODUCT: return "product";
    case NIVALO_OTA_PRECHECK_SECURITY_VERSION: return "security-version";
    case NIVALO_OTA_PRECHECK_SIZE: return "size";
    case NIVALO_OTA_PRECHECK_ORIGIN: return "origin";
    }
    return "?";
}

int signatureCases(JsonObjectConst root)
{
    int checked = 0;
    const std::string pem = pemFromSpkiBase64(str(root["key"]["publicKeySpkiDerBase64"]));
    JsonObjectConst vector = root["vector"].as<JsonObjectConst>();
    const char *signature = str(vector["payload"]["signature"]["value"]);

    context() = "vector canonical bytes";
    char canonical[256];
    const size_t length = nivaloCanonicalManifest(manifestFrom(vector["payload"]["manifest"]), canonical, sizeof(canonical));
    V11_CHECK(std::string(canonical, length) == str(vector["canonicalText"]), canonical);
    V11_CHECK(length == vector["canonicalByteCount"].as<size_t>(), length);
    V11_CHECK(sha256Hex(std::string(canonical, length)) == str(vector["canonicalSha256"]), "canonical digest");
    checked += 3;

    context() = "vector signature";
    V11_CHECK(nivaloStrictDerSignature(signature), "strict DER");
    V11_CHECK(nivaloVerifySignature(pem.c_str(), reinterpret_cast<const uint8_t *>(canonical), length, signature),
              "signature verifies");
    checked += 2;

    for (JsonObjectConst mutation : root["mutations"].as<JsonArrayConst>())
    {
        context() = std::string("mutation ") + str(mutation["id"]);
        std::string text = str(mutation["canonicalText"]);
        if (!mutation["manifest"].isNull())
        {
            char built[256];
            const size_t builtLength = nivaloCanonicalManifest(manifestFrom(mutation["manifest"]), built, sizeof(built));
            V11_CHECK(std::string(built, builtLength) == text, "canonical bytes of the mutated manifest");
        }
        const bool valid = nivaloVerifySignature(pem.c_str(), reinterpret_cast<const uint8_t *>(text.data()), text.size(), signature);
        V11_CHECK(valid == mutation["signatureValid"].as<bool>(), "signature valid " << valid);
        checked++;
    }

    context() = "mock image";
    JsonObjectConst mock = root["mockImage"].as<JsonObjectConst>();
    V11_CHECK(sha256Hex(str(mock["ascii"])) == str(mock["sha256"]), "mock image digest");
    NivaloOperation operation;
    Device device(root["device"].as<JsonObjectConst>(), JsonObjectConst());
    V11_CHECK(parseOta(mock["payload"], &operation) && nivaloOtaPrecheck(device.ota, operation) == NIVALO_OTA_PRECHECK_OK,
              "mock image job passes the prechecks");
    checked += 2;
    return checked;
}

int precheckCases(JsonObjectConst root)
{
    int checked = 0;
    for (JsonObjectConst testCase : root["prechecks"].as<JsonArrayConst>())
    {
        context() = std::string("precheck ") + str(testCase["id"]);
        Device device(root["device"].as<JsonObjectConst>(), testCase["device"].as<JsonObjectConst>());
        NivaloOperation operation;
        const bool parsed = parseOta(testCase["payload"], &operation);
        V11_CHECK(parsed, "precheck payload is a valid OTA job");
        const char *actual = parsed ? precheckName(nivaloOtaPrecheck(device.ota, operation)) : "unparsed";
        V11_CHECK(std::strcmp(actual, str(testCase["expect"])) == 0, actual << " expected " << str(testCase["expect"]));
        checked++;
    }
    return checked;
}

std::vector<uint8_t> filler(size_t count, uint8_t seed)
{
    std::vector<uint8_t> bytes(count);
    uint32_t state = 0x9E3779B9U ^ seed;
    for (size_t i = 0; i < count; i++)
    {
        state = state * 1664525U + 1013904223U;
        bytes[i] = (uint8_t)(state >> 24);
    }
    return bytes;
}

bool scan(const std::vector<uint8_t> &image, size_t chunk, NivaloImageIdentity *identity, unsigned *count)
{
    NivaloImageDescriptorScanner scanner;
    for (size_t offset = 0; offset < image.size(); offset += chunk)
    {
        scanner.update(image.data() + offset, std::min(chunk, image.size() - offset));
    }
    *count = scanner.count();
    return scanner.identity(identity);
}

int descriptorCases()
{
    int checked = 0;
    context() = "running image descriptor";
    const NivaloImageIdentity &running = nivaloRunningImageIdentity();
    V11_CHECK(std::string(running.productId) == "11111111-1111-4111-8111-111111111111" && running.securityVersion == 7U,
              running.productId << " " << running.securityVersion);
    checked++;

    const uint8_t *descriptor = nivaloRunningImageDescriptorBytes();
    std::vector<uint8_t> image = filler(70000U, 1U);
    image.insert(image.begin() + 33333, descriptor, descriptor + NivaloImageDescriptorBytes);
    for (size_t chunk : {1U, 7U, 64U, 4096U})
    {
        context() = std::string("descriptor in an image, chunk ") + std::to_string(chunk);
        NivaloImageIdentity found;
        unsigned count = 0;
        V11_CHECK(scan(image, chunk, &found, &count) && std::string(found.productId) == running.productId &&
                      found.securityVersion == 7U && count == 1U,
                  "count " << count);
        checked++;
    }

    context() = "descriptor with a corrupted check is ignored";
    std::vector<uint8_t> corrupted = image;
    corrupted[33333 + 60] ^= 0x01U;
    NivaloImageIdentity found;
    unsigned count = 0;
    V11_CHECK(!scan(corrupted, 512U, &found, &count) && count == 0U, "count " << count);
    checked++;

    context() = "two descriptors are ambiguous";
    std::vector<uint8_t> twice = image;
    twice.insert(twice.begin() + 60000, descriptor, descriptor + NivaloImageDescriptorBytes);
    V11_CHECK(!scan(twice, 512U, &found, &count) && count == 2U, "count " << count);
    checked++;

    context() = "image without a descriptor";
    V11_CHECK(!scan(filler(50000U, 2U), 512U, &found, &count), "found a descriptor in filler");
    checked++;

    // A stray magic whose 64-byte candidate window covers the start of a second, complete
    // descriptor: a scanner that consumes the window misses that descriptor and counts only the
    // one at 33333. Every magic occurrence counts, overlapping ones included.
    for (size_t offset : {16U, 40U, 63U})
    {
        context() = std::string("descriptor hidden at candidate byte ") + std::to_string(offset);
        std::vector<uint8_t> overlapping = image;
        std::vector<uint8_t> hidden(descriptor, descriptor + 16U);
        hidden.resize(offset, 0x20U);
        hidden.insert(hidden.end(), descriptor, descriptor + NivaloImageDescriptorBytes);
        overlapping.insert(overlapping.begin() + 10000, hidden.begin(), hidden.end());
        NivaloImageDescriptorScanner scanner;
        for (size_t at = 0; at < overlapping.size(); at += 512U)
        {
            scanner.update(overlapping.data() + at, std::min<size_t>(512U, overlapping.size() - at));
        }
        V11_CHECK(scanner.magicCount() == 3U && !scanner.identity(&found), "magic " << scanner.magicCount());
        checked++;
    }
    return checked;
}

int floorCases(JsonObjectConst root)
{
    int checked = 0;
    context() = "H4: an unreadable floor rejects a job between the running version and the lost floor";
    JsonObjectConst mock = root["mockImage"].as<JsonObjectConst>();
    NivaloOperation operation;
    V11_CHECK(parseOta(mock["payload"], &operation), "mock job parses");
    // A fallback image at security version 5 runs; the committed floor (9) cannot be read.
    Device device(root["device"].as<JsonObjectConst>(), JsonObjectConst());
    device.ota.runningSecurityVersion = 5U;
    device.ota.committedFloor = 0U;
    device.ota.floorReadable = false;
    V11_CHECK(operation.manifest.securityVersion > 5U && operation.manifest.securityVersion < 9U, "vector version");
    V11_CHECK(nivaloOtaPrecheck(device.ota, operation) == NIVALO_OTA_PRECHECK_SECURITY_VERSION,
              precheckName(nivaloOtaPrecheck(device.ota, operation)));
    // Read as zero (the former behaviour), the same job would have been accepted.
    device.ota.floorReadable = true;
    V11_CHECK(nivaloOtaPrecheck(device.ota, operation) == NIVALO_OTA_PRECHECK_OK, "control");
    checked++;
    return checked;
}

int recordCases()
{
    int checked = 0;
    context() = "journal blob";
    NivaloJournal journal = nivaloFreshJournal(DeviceId);
    journal.highWater = journal.seq = 43U;
    journal.hasLast = true;
    std::strcpy(journal.commandId, "55555555-5555-4555-8555-55550000002b");
    journal.operationClass = NIVALO_OPERATION_OTA;
    journal.phase = NIVALO_PHASE_CLAIMED;
    uint8_t blob[NivaloJournalBlobBytes];
    const size_t length = nivaloEncodeJournal(journal, blob, sizeof(blob));
    NivaloJournal decoded;
    V11_CHECK(length > 0U && nivaloDecodeJournal(blob, length, &decoded) && decoded.highWater == 43U &&
                  decoded.phase == NIVALO_PHASE_CLAIMED && decoded.operationClass == NIVALO_OPERATION_OTA,
              "round trip");
    // Every single-bit corruption is detected, so a damaged journal never yields a usable record.
    bool allDetected = true;
    for (size_t bit = 0; bit < length * 8U; bit++)
    {
        uint8_t damaged[NivaloJournalBlobBytes];
        std::memcpy(damaged, blob, length);
        damaged[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        allDetected = allDetected && !nivaloDecodeJournal(damaged, length, &decoded);
    }
    V11_CHECK(allDetected, "a corrupted journal decoded");
    V11_CHECK(!nivaloDecodeJournal(blob, length - 1U, &decoded), "truncated journal decoded");
    checked += 3;

    context() = "pending and floor blobs";
    NivaloPendingRecord pending;
    pending.commandSeq = 43U;
    std::strcpy(pending.productId, "11111111-1111-4111-8111-111111111111");
    pending.securityVersion = 7U;
    pending.sizeBytes = 100U;
    std::strcpy(pending.sha256, "b729e6df1dcaa6ad607e4cef49f8975483f944794e73dc385c2aa16c747fda0b");
    uint8_t pendingBlob[NivaloPendingBlobBytes];
    const size_t pendingLength = nivaloEncodePending(pending, pendingBlob, sizeof(pendingBlob));
    NivaloPendingRecord pendingDecoded;
    V11_CHECK(pendingLength > 0U && nivaloDecodePending(pendingBlob, pendingLength, &pendingDecoded) &&
                  pendingDecoded.sizeBytes == 100U && std::string(pendingDecoded.sha256) == pending.sha256,
              "pending round trip");
    pendingBlob[10] ^= 0x40U;
    V11_CHECK(!nivaloDecodePending(pendingBlob, pendingLength, &pendingDecoded), "corrupted pending decoded");
    NivaloFloorRecord floor;
    std::strcpy(floor.productId, pending.productId);
    floor.floor = 7U;
    uint8_t floorBlob[NivaloFloorBlobBytes];
    const size_t floorLength = nivaloEncodeFloor(floor, floorBlob, sizeof(floorBlob));
    NivaloFloorRecord floorDecoded;
    V11_CHECK(floorLength > 0U && nivaloDecodeFloor(floorBlob, floorLength, &floorDecoded) && floorDecoded.floor == 7U,
              "floor round trip");
    checked += 3;

    context() = "origins";
    NivaloOrigin origin;
    V11_CHECK(nivaloParseHttpsOrigin("https://iot-api.nivalo.io", false, &origin) && origin.port == 443U, "configured origin");
    V11_CHECK(!nivaloParseHttpsOrigin("https://iot-api.nivalo.io", true, &origin), "download URL needs a path");
    V11_CHECK(!nivaloParseHttpsOrigin("http://iot-api.nivalo.io/x", true, &origin), "http");
    V11_CHECK(!nivaloParseHttpsOrigin("https://user@iot-api.nivalo.io/x", true, &origin), "userinfo");
    V11_CHECK(!nivaloParseHttpsOrigin("https://iot-api.nivalo.io:65536/x", true, &origin), "port out of range");
    V11_CHECK(!nivaloParseHttpsOrigin("https://iot-api.nivalo.io:0443/x", true, &origin), "leading zero port");
    checked += 6;
    return checked;
}
} // namespace

int main()
{
    std::unique_ptr<DynamicJsonDocument> document = loadFixture("fixtures/v1.1/vectors/ota.json");
    JsonObjectConst root = document->as<JsonObjectConst>();
    int checked = signatureCases(root);
    checked += precheckCases(root);
    checked += descriptorCases();
    checked += floorCases(root);
    checked += recordCases();
    return finish("OTA vectors", checked);
}
