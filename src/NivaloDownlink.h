#ifndef NIVALO_DOWNLINK_H
#define NIVALO_DOWNLINK_H

// Receiver for commands/{id} and ota/{id} (specs/cloud-mqtt-v1.1.md sections 3, 4, 10.4, 11).

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "NivaloWire.h"

struct NivaloOtaManifest
{
    char productId[37] = {0};
    uint32_t securityVersion = 0U;
    uint32_t sizeBytes = 0U;
    char sha256[65] = {0};
};

// One identified operation. Everything the executor needs is copied out of the MQTT buffer.
struct NivaloOperation
{
    NivaloOperationClass operationClass = NIVALO_OPERATION_COMMAND;
    char id[37] = {0};
    uint32_t seq = 0U;
    // SHA-256 of the exact bytes of the envelope's payload value as received.
    uint8_t fingerprint[32] = {0};
    // Payload schema, lifetime and (OTA) strict DER signature encoding all pass.
    bool valid = false;
    int64_t issuedAtMs = 0;
    int64_t expiresAtMs = 0;
    // Generic command. arguments holds the exact bytes of payload.arguments as received (a JSON
    // object, NUL-terminated); they are parsed once, by the handler's preparation, never
    // re-serialized.
    char command[65] = {0};
    char arguments[NivaloV11::MaxCommandBytes] = {0};
    size_t argumentsLength = 0U;
    // OTA job.
    NivaloOtaManifest manifest;
    char url[513] = {0};
    char keyId[65] = {0};
    char signature[97] = {0};
};

enum NivaloDownlinkOutcome
{
    // Not identified: dropped without an ACK.
    NIVALO_DOWNLINK_DROPPED = 0,
    // Identified: topic, size, encoding, JSON, complexity, envelope, device ID, correlation and
    // commandSeq are valid. operation->valid tells whether the rest is: the payload schema and
    // lifetime, firmware placement (no envelope firmware on an operation route) and, for OTA, the
    // strict DER signature encoding.
    NIVALO_DOWNLINK_IDENTIFIED = 1
};

// Parses one downlink without modifying payload. topics must be configured for this device.
NivaloDownlinkOutcome nivaloParseDownlink(const NivaloTopics &topics, const char *topic, const uint8_t *payload,
                                          size_t length, NivaloOperation *operation);

// Schema checks, exposed for the conformance tests.
bool nivaloValidEnvelope(JsonVariantConst envelope);
bool nivaloValidFirmware(JsonVariantConst firmware);
bool nivaloValidCommandPayload(JsonVariantConst payload);
bool nivaloValidOtaPayload(JsonVariantConst payload);
bool nivaloValidOtaManifest(JsonVariantConst manifest);
bool nivaloValidPositiveUint32(JsonVariantConst value);
bool nivaloValidUtcValue(JsonVariantConst value);
bool nivaloValidUuidValue(JsonVariantConst value);
bool nivaloValidNameValue(JsonVariantConst value);
bool nivaloValidFunctionNameValue(JsonVariantConst value);
bool nivaloValidTextValue(JsonVariantConst value, size_t maxBytes);
// Command arguments: an object without null at any depth.
bool nivaloValidCommandArguments(JsonVariantConst arguments);

#endif
