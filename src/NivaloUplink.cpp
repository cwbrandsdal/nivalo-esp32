#include "NivaloUplink.h"

#include <string.h>

#include "NivaloJsonStrict.h"

namespace
{
// Envelope root (level 1) and payload (level 2) leave six levels for event data and state values.
const unsigned EmbeddedValueDepth = NivaloV11::MaxDepth - 2U;

bool strictJson(const char *json, unsigned maxDepth)
{
    return json != NULL && nivaloScanJson(json, strlen(json), maxDepth, NivaloV11::MaxValues, NULL) == NIVALO_JSON_OK;
}

size_t finish(NivaloJsonWriter &writer)
{
    return writer.failed() ? 0U : writer.length();
}

bool validSeverity(const char *severity)
{
    static const char *const Severities[] = {"debug", "info", "warning", "error", "critical"};
    for (size_t i = 0U; severity != NULL && i < sizeof(Severities) / sizeof(Severities[0]); i++)
    {
        if (strcmp(severity, Severities[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

bool reservedEventName(const char *name)
{
    static const char Prefix[] = "state.";
    for (size_t i = 0U; i < sizeof(Prefix) - 1U; i++)
    {
        char c = name[i];
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        if (c != Prefix[i])
        {
            return false;
        }
    }
    return true;
}

void writeEpoch(NivaloJsonWriter &writer, const char *kind, const char *status, const NivaloEpoch &epoch)
{
    writer.beginObject();
    writer.key("kind");
    writer.string(kind);
    writer.key("status");
    writer.string(status);
    writer.key("boot");
    writer.uint(epoch.boot);
    writer.key("connection");
    writer.uint(epoch.connection);
}

bool validEpoch(const NivaloEpoch &epoch) { return epoch.boot > 0U && epoch.connection > 0U; }
} // namespace

bool nivaloValidFirmwareInfo(const NivaloFirmwareInfo &firmware)
{
    return nivaloIsText(firmware.version, 64U) && nivaloIsText(firmware.hardware, 64U);
}

size_t nivaloBuildEnvelope(const char *deviceId, const char *messageId, int64_t sentAtMs, const char *correlationId,
                           const NivaloFirmwareInfo *firmware, const char *payload, size_t payloadLength,
                           char *output, size_t capacity)
{
    char sentAt[NivaloV11::UtcCapacity];
    if (!nivaloIsUuid(deviceId) || !nivaloIsUuid(messageId) || !nivaloFormatUtc(sentAtMs, sentAt, sizeof(sentAt)) ||
        (correlationId != NULL && !nivaloIsUuid(correlationId)) ||
        (firmware != NULL && !nivaloValidFirmwareInfo(*firmware)) || payload == NULL || payloadLength == 0U ||
        payload[0] != '{')
    {
        return 0U;
    }
    NivaloJsonWriter writer(output, capacity);
    writer.beginObject();
    writer.key("schema");
    writer.string(NivaloV11::SchemaMarker);
    writer.key("messageId");
    writer.string(messageId);
    writer.key("deviceId");
    writer.string(deviceId);
    writer.key("sentAt");
    writer.string(sentAt);
    if (correlationId != NULL)
    {
        writer.key("correlationId");
        writer.string(correlationId);
    }
    if (firmware != NULL)
    {
        writer.key("firmware");
        writer.beginObject();
        writer.key("version");
        writer.string(firmware->version);
        writer.key("hardware");
        writer.string(firmware->hardware);
        writer.key("targets");
        writer.beginArray();
        // The STM32 bridge does not advertise MCU execution or OTA in v1.1.
        writer.string("esp32");
        writer.endArray();
        writer.endObject();
    }
    writer.key("payload");
    writer.raw(payload, payloadLength);
    writer.endObject();
    const size_t length = finish(writer);
    if (length == 0U || length > NivaloV11::MaxUplinkBytes)
    {
        return 0U;
    }
    NivaloJsonScan scan;
    if (nivaloScanJson(output, length, NivaloV11::MaxDepth, NivaloV11::MaxValues, &scan) != NIVALO_JSON_OK ||
        !scan.rootIsObject || !scan.hasPayload)
    {
        return 0U;
    }
    return length;
}

size_t nivaloBirthPayload(const NivaloEpoch &epoch, char *output, size_t capacity)
{
    if (!validEpoch(epoch))
    {
        return 0U;
    }
    NivaloJsonWriter writer(output, capacity);
    writeEpoch(writer, "birth", "online", epoch);
    writer.key("seq");
    writer.uint(0U);
    writer.endObject();
    return finish(writer);
}

size_t nivaloHeartbeatPayload(const NivaloEpoch &epoch, uint32_t seq, const NivaloHealth &health, char *output,
                              size_t capacity)
{
    if (!validEpoch(epoch) || seq == 0U || seq == 0xFFFFFFFFUL || health.heapFreeB > health.heapTotalB ||
        (health.hasRssi && (health.wifiRssiDbm < -127 || health.wifiRssiDbm > 0)))
    {
        return 0U;
    }
    NivaloJsonWriter writer(output, capacity);
    writeEpoch(writer, "heartbeat", "online", epoch);
    writer.key("seq");
    writer.uint(seq);
    writer.key("uptimeS");
    writer.uint(health.uptimeS);
    writer.key("heapFreeB");
    writer.uint(health.heapFreeB);
    writer.key("heapTotalB");
    writer.uint(health.heapTotalB);
    if (health.hasRssi)
    {
        writer.key("wifiRssiDbm");
        writer.integer(health.wifiRssiDbm);
    }
    writer.key("droppedBatches");
    writer.uint(health.droppedBatches);
    writer.endObject();
    return finish(writer);
}

size_t nivaloEndPayload(const NivaloEpoch &epoch, bool shutdown, char *output, size_t capacity)
{
    if (!validEpoch(epoch))
    {
        return 0U;
    }
    NivaloJsonWriter writer(output, capacity);
    writeEpoch(writer, "end", "offline", epoch);
    writer.key("reason");
    writer.string(shutdown ? "shutdown" : "will");
    writer.endObject();
    return finish(writer);
}

size_t nivaloAckPayload(uint32_t seq, const NivaloResult &result, const char *message, int progress, char *output,
                        size_t capacity)
{
    const char *status = nivaloAckStatusName(result.status);
    const bool needsCode = result.status == NIVALO_ACK_FAILED || result.status == NIVALO_ACK_INDETERMINATE;
    const char *code = nivaloAckCodeName(result.code);
    const bool failureCode = result.code >= NIVALO_CODE_EXPIRED && result.code <= NIVALO_CODE_HANDLER_FAILED;
    const bool uncertaintyCode = result.code == NIVALO_CODE_EXECUTION_UNKNOWN || result.code == NIVALO_CODE_REPLAY_SUPPRESSED;
    if (seq == 0U || status == NULL || needsCode != (code != NULL) ||
        (result.status == NIVALO_ACK_FAILED && !failureCode) ||
        (result.status == NIVALO_ACK_INDETERMINATE && !uncertaintyCode) ||
        (message != NULL && !nivaloIsText(message, NivaloV11::MaxAckMessageBytes)) ||
        (progress >= 0 && (result.status != NIVALO_ACK_RUNNING || progress > 100)))
    {
        return 0U;
    }
    NivaloJsonWriter writer(output, capacity);
    writer.beginObject();
    writer.key("commandSeq");
    writer.uint(seq);
    writer.key("status");
    writer.string(status);
    if (code != NULL)
    {
        writer.key("code");
        writer.string(code);
    }
    if (message != NULL)
    {
        writer.key("message");
        writer.string(message);
    }
    if (progress >= 0)
    {
        writer.key("progress");
        writer.uint((uint32_t)progress);
    }
    writer.endObject();
    return finish(writer);
}

size_t nivaloEventPayload(const char *name, const char *severity, const char *dataJson, const char *raw,
                          char *output, size_t capacity)
{
    if (!nivaloIsName(name) || reservedEventName(name) || !validSeverity(severity) ||
        (dataJson != NULL && raw != NULL) || (dataJson != NULL && !strictJson(dataJson, EmbeddedValueDepth)) ||
        (raw != NULL && (strlen(raw) > NivaloV11::MaxEventRawBytes || !nivaloIsUtf8(raw, strlen(raw)))))
    {
        return 0U;
    }
    NivaloJsonWriter writer(output, capacity);
    writer.beginObject();
    writer.key("name");
    writer.string(name);
    writer.key("severity");
    writer.string(severity);
    if (dataJson != NULL)
    {
        writer.key("data");
        writer.raw(dataJson, strlen(dataJson));
    }
    if (raw != NULL)
    {
        writer.key("raw");
        writer.string(raw);
    }
    writer.endObject();
    return finish(writer);
}

size_t nivaloStatePayload(const char *name, const char *valueJson, char *output, size_t capacity)
{
    if (!nivaloIsName(name) || !strictJson(valueJson, EmbeddedValueDepth))
    {
        return 0U;
    }
    NivaloJsonWriter writer(output, capacity);
    writer.beginObject();
    writer.key("name");
    writer.string(name);
    writer.key("value");
    writer.raw(valueJson, strlen(valueJson));
    writer.endObject();
    return finish(writer);
}

// --- Telemetry batch ---------------------------------------------------------------------------

NivaloTelemetryBatch::NivaloTelemetryBatch()
{
    reset();
}

void NivaloTelemetryBatch::reset()
{
    static const char Prefix[] = "{\"samples\":[";
    memcpy(_buffer, Prefix, sizeof(Prefix));
    _length = sizeof(Prefix) - 1U;
    _samples = 0U;
    _readings = 0U;
    _sampleMetrics = 0U;
    memset(_sampleCounts, 0, sizeof(_sampleCounts));
    _lastAtMs = 0;
    _openAtMs = 0;
    _closed = false;
}

bool NivaloTelemetryBatch::beginSample(int64_t atMs)
{
    char at[NivaloV11::UtcCapacity];
    if (_closed || !nivaloFormatUtc(atMs, at, sizeof(at)))
    {
        return false;
    }
    // An open sample without metrics simply moves to the new time.
    dropEmptySample();
    if (_samples >= NivaloV11::MaxSamples || (_samples > 0U && atMs <= _lastAtMs))
    {
        return false;
    }
    char text[64];
    NivaloJsonWriter writer(text, sizeof(text));
    writer.beginObject();
    writer.key("at");
    writer.string(at);
    writer.key("metrics");
    const size_t closing = _samples > 0U ? 2U : 0U; // "]}" of the previous sample
    const size_t addition = closing + (_samples > 0U ? 1U : 0U) + writer.length() + 1U;
    // Room for the addition plus the closers of this sample and of the batch.
    if (writer.failed() || _length + addition + 4U > PayloadCapacity)
    {
        return false;
    }
    if (_samples > 0U)
    {
        memcpy(_buffer + _length, "]},", 3U);
        _length += 3U;
    }
    memcpy(_buffer + _length, writer.c_str(), writer.length());
    _length += writer.length();
    _buffer[_length++] = '[';
    _buffer[_length] = '\0';
    _samples++;
    _sampleMetrics = 0U;
    _openAtMs = atMs;
    return true;
}

void NivaloTelemetryBatch::dropEmptySample()
{
    if (_samples == 0U || _sampleMetrics > 0U)
    {
        return;
    }
    // The open sample is the last object in the buffer and has no metrics yet. Cut it, together
    // with the "]}," that closed the previous sample, which becomes the open one again.
    const char *open = strrchr(_buffer, '{');
    _length = (size_t)(open - _buffer);
    _samples--;
    if (_samples > 0U)
    {
        _length -= 3U;
        _sampleMetrics = _sampleCounts[_samples - 1U];
    }
    _buffer[_length] = '\0';
}

bool NivaloTelemetryBatch::addValue(const char *name, double value, const char *unit)
{
    char text[32];
    if (!nivaloFormatDecimal(value, text, sizeof(text)))
    {
        return false;
    }
    return addMetric(name, text, NULL, unit);
}

bool NivaloTelemetryBatch::addRaw(const char *name, const char *raw, const char *unit)
{
    if (raw == NULL || strlen(raw) > NivaloV11::MaxRawBytes || !nivaloIsUtf8(raw, strlen(raw)))
    {
        return false;
    }
    return addMetric(name, NULL, raw, unit);
}

bool NivaloTelemetryBatch::addMetric(const char *name, const char *valueText, const char *raw, const char *unit)
{
    if (_closed || _samples == 0U || _readings >= NivaloV11::MaxReadings || !nivaloIsName(name) ||
        (unit != NULL && !nivaloIsText(unit, NivaloV11::MaxUnitBytes)))
    {
        return false;
    }
    for (unsigned i = 0U; i < _sampleMetrics; i++)
    {
        if (nivaloAsciiCaseEqual(_names[_readings - _sampleMetrics + i], name))
        {
            return false;
        }
    }
    // Written in place after the current content, leaving room for the closing "]}]}".
    const size_t start = _length;
    const size_t offset = start + (_sampleMetrics > 0U ? 1U : 0U);
    if (offset + 4U >= PayloadCapacity)
    {
        return false;
    }
    NivaloJsonWriter writer(_buffer + offset, PayloadCapacity - 4U - offset + 1U);
    writer.beginObject();
    writer.key("name");
    writer.string(name);
    if (valueText != NULL)
    {
        writer.key("value");
        writer.raw(valueText, strlen(valueText));
    }
    else
    {
        writer.key("raw");
        writer.string(raw);
    }
    if (unit != NULL)
    {
        writer.key("unit");
        writer.string(unit);
    }
    writer.endObject();
    if (writer.failed())
    {
        _buffer[start] = '\0';
        return false;
    }
    if (_sampleMetrics > 0U)
    {
        _buffer[start] = ',';
    }
    _length = offset + writer.length();
    _buffer[_length] = '\0';
    memcpy(_names[_readings], name, strlen(name) + 1U);
    _readings++;
    _sampleMetrics++;
    _sampleCounts[_samples - 1U] = (uint8_t)_sampleMetrics;
    _lastAtMs = _openAtMs;
    return true;
}

const char *NivaloTelemetryBatch::payload(size_t *length)
{
    if (!_closed)
    {
        dropEmptySample();
        if (_samples == 0U)
        {
            return NULL;
        }
        memcpy(_buffer + _length, "]}]}", 4U);
        _length += 4U;
        _buffer[_length] = '\0';
        _closed = true;
    }
    if (length != NULL)
    {
        *length = _length;
    }
    return _buffer;
}
