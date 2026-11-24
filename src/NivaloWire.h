#ifndef NIVALO_WIRE_H
#define NIVALO_WIRE_H

// Wire primitives of Nivalo cloud MQTT v1.1 (nivalo-protocol specs/cloud-mqtt-v1.1.md).
// Arduino-free so the host tests drive the same code as the firmware.

#include <stddef.h>
#include <stdint.h>

namespace NivaloV11
{
static const char SchemaMarker[] = "nivalo.iot.v1.1";
static const char TopicRoot[] = "nivalo/v1.1/devices/";

// Section 4.2.
static const size_t MaxUplinkBytes = 3072U;
static const size_t MaxCommandBytes = 1536U;
static const size_t MaxOtaBytes = 2048U;
static const unsigned MaxDepth = 8U;
static const unsigned MaxValues = 192U;
static const unsigned MaxSamples = 8U;
static const unsigned MaxReadings = 24U;
static const size_t MaxRawBytes = 512U;
static const size_t MaxUnitBytes = 32U;
static const size_t MaxEventRawBytes = 1024U;
static const size_t MaxAckMessageBytes = 95U;
static const int64_t GenericLifetimeMs = 900000;
static const int64_t OtaLifetimeMs = 7200000;
// Section 10.4 step 2.
static const int64_t IssuedAheadMs = 2000;
// Section 8.1.
static const uint32_t HeartbeatIntervalMs = 600000UL;
static const uint32_t HeartbeatJitterMs = 30000UL;

static const size_t UuidLength = 36U;
static const size_t MaxNameLength = 64U;
static const size_t UtcCapacity = 25U; // YYYY-MM-DDTHH:MM:SS.mmmZ plus NUL
} // namespace NivaloV11

// --- Text and identifiers ----------------------------------------------------------------------

// Canonical lowercase hyphenated UUID, never all zeros (common#/$defs/uuid).
bool nivaloIsUuid(const char *value);
bool nivaloIsUuid(const char *value, size_t length);
// Name pattern ^[A-Za-z][A-Za-z0-9_.-]{0,63}$ (common#/$defs/name).
bool nivaloIsName(const char *value);
// flash, requestDefinitions, firmware.* and ota.* in any letter case.
bool nivaloIsReservedFunctionName(const char *value);
// A reported function name: a name that is not reserved.
bool nivaloIsFunctionName(const char *value);
// Text (common#/$defs/text) of at most maxBytes UTF-8 bytes: valid UTF-8, no leading or
// trailing space, no C0 control or DEL anywhere.
bool nivaloIsText(const char *value, size_t maxBytes);
// Well-formed UTF-8 (RFC 3629) without U+0000.
bool nivaloIsUtf8(const char *value, size_t length);
// Decodes one well-formed UTF-8 sequence at *index and advances it.
bool nivaloUtf8Next(const char *value, size_t length, size_t *index, uint32_t *codePoint);
// Two names that differ only in ASCII letter case.
bool nivaloAsciiCaseEqual(const char *left, const char *right);
// Lowercase hex digits only, exactly length characters.
bool nivaloIsLowerHex(const char *value, size_t length);

// --- Time --------------------------------------------------------------------------------------

// common#/$defs/utc: YYYY-MM-DDTHH:MM:SS[.mmm]Z on a real calendar date in 2020-2037.
bool nivaloParseUtc(const char *value, int64_t *epochMs);
// Formats epochMs with exactly three fractional digits. False outside 2020-2037.
bool nivaloFormatUtc(int64_t epochMs, char *output, size_t capacity);

// --- Identifiers -------------------------------------------------------------------------------

// Random UUID v4 from 16 random bytes, canonical lowercase.
void nivaloFormatUuidV4(const uint8_t random[16], char output[37]);
// The last eight hex digits of an operation ID encode commandSeq (section 4.3).
bool nivaloSeqMatchesId(uint32_t seq, const char *operationId);

// --- Bounded JSON writer -----------------------------------------------------------------------

// Writes compact JSON into a caller buffer. Any overflow or misuse sets failed(); the output is
// then incomplete and must not be published.
class NivaloJsonWriter
{
public:
    NivaloJsonWriter(char *buffer, size_t capacity);
    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(const char *name);
    void string(const char *value);
    void string(const char *value, size_t length);
    void uint(uint64_t value);
    void integer(int64_t value);
    void boolean(bool value);
    // Inserts already-validated JSON text verbatim as one value.
    void raw(const char *json, size_t length);
    // Exact decimal with at most six fractional digits; false if not finite or out of range.
    bool decimal(double value);

    bool failed() const { return _failed; }
    size_t length() const { return _length; }
    const char *c_str() const { return _buffer; }

private:
    void separator();
    void quoted(const char *value, size_t length);
    void put(char c);
    void put(const char *text, size_t length);
    char *_buffer;
    size_t _capacity;
    size_t _length = 0U;
    bool _failed = false;
    bool _needComma = false;
    bool _afterKey = false;
    unsigned _depth = 0U;
};

// Formats value as an exact decimal (no exponent, at most six fractional digits, trailing zeros
// trimmed) within +-4294967295.999999 (telemetry value, section 4.2).
bool nivaloFormatDecimal(double value, char *output, size_t capacity);

// --- Topics ------------------------------------------------------------------------------------

enum NivaloOperationClass
{
    NIVALO_OPERATION_COMMAND = 0,
    NIVALO_OPERATION_OTA = 1
};

class NivaloTopics
{
public:
    // deviceId must be a canonical UUID.
    bool configure(const char *deviceId);
    const char *deviceId() const { return _deviceId; }
    const char *telemetry() const { return _telemetry; }
    const char *events() const { return _events; }
    const char *state() const { return _state; }
    const char *definitions() const { return _definitions; }
    const char *availability() const { return _availability; }
    const char *commandsFilter() const { return _commandsFilter; }
    const char *otaFilter() const { return _otaFilter; }
    // commands/{id}/ack or ota/{id}/ack.
    bool ack(NivaloOperationClass operationClass, const char *operationId, char *output, size_t capacity) const;
    // Exactly nivalo/v1.1/devices/{deviceId}/commands/{uuid} or .../ota/{uuid}.
    bool parseDownlink(const char *topic, NivaloOperationClass *operationClass, char operationId[37]) const;

private:
    char _deviceId[37] = {0};
    char _prefix[64] = {0};
    char _telemetry[80] = {0};
    char _events[80] = {0};
    char _state[80] = {0};
    char _definitions[80] = {0};
    char _availability[80] = {0};
    char _commandsFilter[80] = {0};
    char _otaFilter[80] = {0};
};

#endif
