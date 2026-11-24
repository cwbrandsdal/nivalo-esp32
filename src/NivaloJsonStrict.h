#ifndef NIVALO_JSON_STRICT_H
#define NIVALO_JSON_STRICT_H

// Strict single-pass JSON scanner for v1.1 wire payloads (specs/cloud-mqtt-v1.1.md section 4.1).
// ArduinoJson is lenient (unquoted keys, duplicate members, NUL), so every received payload and
// every payload the device builds is scanned first. The scanner works on the caller's read-only
// bytes, so it can run while PubSubClient's buffer is still intact.

#include <stddef.h>
#include <stdint.h>

enum NivaloJsonRule
{
    NIVALO_JSON_OK = 0,
    NIVALO_JSON_ENCODING,         // BOM, malformed UTF-8, U+0000 or an unpaired surrogate
    NIVALO_JSON_SYNTAX,           // not exactly one RFC 8259 value
    NIVALO_JSON_DUPLICATE_MEMBER, // an object repeats a member name (after unescaping)
    NIVALO_JSON_DEPTH,            // containers nested deeper than maxDepth (root container is 1)
    NIVALO_JSON_VALUE_COUNT       // more than maxValues values (member names excluded)
};

struct NivaloJsonScan
{
    NivaloJsonRule rule = NIVALO_JSON_SYNTAX;
    bool rootIsObject = false;
    // The exact bytes of the root object's member named "payload", as received (section 10.4).
    bool hasPayload = false;
    size_t payloadOffset = 0U;
    size_t payloadLength = 0U;
    unsigned depth = 0U;
    unsigned values = 0U;
};

// Scans length bytes (at most 65535). maxValues may be at most NivaloJsonMaxValues.
NivaloJsonRule nivaloScanJson(const char *data, size_t length, unsigned maxDepth, unsigned maxValues,
                              NivaloJsonScan *scan);

static const unsigned NivaloJsonMaxValues = 192U;
static const unsigned NivaloJsonMaxDepth = 16U;

#endif
