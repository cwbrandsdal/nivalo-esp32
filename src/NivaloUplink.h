#ifndef NIVALO_UPLINK_H
#define NIVALO_UPLINK_H

// Builders for every message the device publishes (specs/cloud-mqtt-v1.1.md sections 3-8, 10.5
// and 12). Each complete envelope is re-scanned with the strict scanner and checked against the
// 3072-byte limit before it is handed to MQTT, so an application value can never produce a
// message the gateway would reject as a whole.

#include <stddef.h>
#include <stdint.h>

#include "NivaloJournal.h"
#include "NivaloWire.h"

struct NivaloFirmwareInfo
{
    // Text of 1-64 bytes each. targets is always ["esp32"] in this profile.
    const char *version = NULL;
    const char *hardware = NULL;
};

bool nivaloValidFirmwareInfo(const NivaloFirmwareInfo &firmware);

// Envelope around payload (an object). correlationId and firmware may be NULL. Returns the
// envelope length, or 0 if it is invalid, too large or does not fit output.
size_t nivaloBuildEnvelope(const char *deviceId, const char *messageId, int64_t sentAtMs, const char *correlationId,
                           const NivaloFirmwareInfo *firmware, const char *payload, size_t payloadLength,
                           char *output, size_t capacity);

// --- Availability (section 8.1) ----------------------------------------------------------------

struct NivaloEpoch
{
    uint32_t boot = 0U;
    uint32_t connection = 0U;
};

struct NivaloHealth
{
    uint32_t uptimeS = 0U;
    uint32_t heapFreeB = 0U;
    uint32_t heapTotalB = 0U;
    bool hasRssi = false;
    int32_t wifiRssiDbm = 0;
    uint32_t droppedBatches = 0U;
};

size_t nivaloBirthPayload(const NivaloEpoch &epoch, char *output, size_t capacity);
size_t nivaloHeartbeatPayload(const NivaloEpoch &epoch, uint32_t seq, const NivaloHealth &health, char *output,
                              size_t capacity);
size_t nivaloEndPayload(const NivaloEpoch &epoch, bool shutdown, char *output, size_t capacity);

// --- ACK (section 10.5) ------------------------------------------------------------------------

// message (<= 95 bytes of text) and progress (0-100, running only; -1 for none) are optional.
size_t nivaloAckPayload(uint32_t seq, const NivaloResult &result, const char *message, int progress, char *output,
                        size_t capacity);

// --- Telemetry batch (section 5) ---------------------------------------------------------------

class NivaloTelemetryBatch
{
public:
    // Payload budget that leaves room for the envelope within the 3072-byte limit.
    static const size_t PayloadCapacity = 2880U;

    NivaloTelemetryBatch();
    void reset();
    bool empty() const { return _samples == 0U; }
    unsigned samples() const { return _samples; }
    unsigned readings() const { return _readings; }

    // Opens a sample observed at atMs. False when the batch has eight samples or atMs is not
    // after the previous sample: publish the batch first.
    bool beginSample(int64_t atMs);
    // Adds one metric to the open sample. False (and the batch unchanged) for an invalid name,
    // unit or value, a duplicate or case-twin name in the sample, or a full batch.
    bool addValue(const char *name, double value, const char *unit);
    bool addRaw(const char *name, const char *raw, const char *unit);
    // {"samples":[...]}; NULL when empty.
    const char *payload(size_t *length);

private:
    bool addMetric(const char *name, const char *valueText, const char *raw, const char *unit);
    void dropEmptySample();
    char _buffer[PayloadCapacity + 1U];
    size_t _length = 0U;
    unsigned _samples = 0U;
    unsigned _readings = 0U;
    unsigned _sampleMetrics = 0U;
    uint8_t _sampleCounts[NivaloV11::MaxSamples] = {0};
    // Time of the newest sample with metrics, and of the open one.
    int64_t _lastAtMs = 0;
    int64_t _openAtMs = 0;
    char _names[NivaloV11::MaxReadings][65];
    bool _closed = false;
};

// --- Events and state (section 6) --------------------------------------------------------------

// data is strict JSON text (any value, including null) or NULL; raw is text of at most 1024
// bytes or NULL; at most one of them. severity is debug, info, warning, error or critical.
size_t nivaloEventPayload(const char *name, const char *severity, const char *dataJson, const char *raw,
                          char *output, size_t capacity);
// value is strict JSON text (any value, including null).
size_t nivaloStatePayload(const char *name, const char *valueJson, char *output, size_t capacity);

#endif
