#include "NivaloDevice.h"

#include <WiFi.h>
#include <math.h>
#include <stdlib.h>

#include "NivaloJsonStrict.h"

namespace
{
// A decimal number as written by an application ("21.5", "-3", "1e3"); anything else is raw.
bool parseNumber(const char *text, double *value)
{
    if (text == NULL || !(text[0] == '-' || (text[0] >= '0' && text[0] <= '9')))
    {
        return false;
    }
    NivaloJsonScan scan;
    if (nivaloScanJson(text, strlen(text), 1U, 1U, &scan) != NIVALO_JSON_OK)
    {
        return false;
    }
    char *end = NULL;
    *value = strtod(text, &end);
    return end != NULL && *end == '\0' && isfinite(*value);
}
} // namespace

bool NivaloDevice::publishEnvelope(const char *topic, const char *payload, size_t length, const char *correlationId,
                                   const NivaloFirmwareInfo *firmware, bool retained)
{
    // Without a valid clock there is no honest sentAt, so nothing is sent (section 3).
    if (!_connection.connected() || !clockValid() || topic == NULL)
    {
        return false;
    }
    char messageId[37];
    nextMessageId(messageId);
    const size_t envelope = nivaloBuildEnvelope(_deviceId.c_str(), messageId, _clock.read(monotonicUs()).utcMs,
                                                correlationId, firmware, payload, length, _envelope, sizeof(_envelope));
    return envelope > 0U && _connection.publish(topic, _envelope, retained);
}

// --- ACKs ----------------------------------------------------------------------------------------

bool NivaloDevice::publishAckNow(const NivaloAck &ack)
{
    char topic[128];
    char payload[160];
    const size_t length = nivaloAckPayload(ack.seq, ack.result, NULL, -1, payload, sizeof(payload));
    return length > 0U && _topics.ack(ack.operationClass, ack.id, topic, sizeof(topic)) &&
           publishEnvelope(topic, payload, length, ack.id, NULL, false);
}

void NivaloDevice::queueAck(const NivaloAck &ack)
{
    for (size_t i = 0U; i < _ackQueueCount; i++)
    {
        if (_ackQueue[i].seq == ack.seq && _ackQueue[i].operationClass == ack.operationClass &&
            _ackQueue[i].result.status == ack.result.status && _ackQueue[i].result.code == ack.result.code &&
            strcmp(_ackQueue[i].id, ack.id) == 0)
        {
            return;
        }
    }
    if (_ackQueueCount == AckQueueCapacity)
    {
        // A dropped terminal ACK is repeated from the journal with the next birth or heartbeat.
        memmove(&_ackQueue[0], &_ackQueue[1], sizeof(NivaloAck) * (AckQueueCapacity - 1U));
        _ackQueueCount--;
    }
    _ackQueue[_ackQueueCount++] = ack;
}

void NivaloDevice::drainAcks()
{
    while (_ackQueueCount > 0U && _connection.connected() && clockValid())
    {
        if (!publishAckNow(_ackQueue[0]))
        {
            return;
        }
        memmove(&_ackQueue[0], &_ackQueue[1], sizeof(NivaloAck) * (_ackQueueCount - 1U));
        _ackQueueCount--;
    }
}

// --- Telemetry -----------------------------------------------------------------------------------

void NivaloDevice::bufferTelemetry(const char *envelope, size_t length)
{
    if (_telemetryBufferCapacity == 0U)
    {
        _droppedBatches++;
        return;
    }
    if (_telemetryBufferCount == _telemetryBufferCapacity)
    {
        _telemetryBuffer[_telemetryBufferHead] = "";
        _telemetryBufferHead = (_telemetryBufferHead + 1U) % 8U;
        _telemetryBufferCount--;
        _droppedBatches++;
    }
    const uint8_t tail = (_telemetryBufferHead + _telemetryBufferCount) % 8U;
    _telemetryBuffer[tail] = String(envelope).substring(0, length);
    _telemetryBufferCount++;
}

void NivaloDevice::flushTelemetryBuffer()
{
    // A retried message keeps its bytes, including messageId and sentAt (section 3).
    while (_connection.connected() && _telemetryBufferCount > 0U)
    {
        String &entry = _telemetryBuffer[_telemetryBufferHead];
        if (!_connection.publish(_topics.telemetry(), entry.c_str(), false))
        {
            return;
        }
        entry = "";
        _telemetryBufferHead = (_telemetryBufferHead + 1U) % 8U;
        _telemetryBufferCount--;
    }
}

bool NivaloDevice::publishBatch()
{
    size_t length = 0U;
    const char *payload = _batch.payload(&length);
    bool published = false;
    if (payload != NULL)
    {
        if (!clockValid())
        {
            _droppedBatches++;
        }
        else
        {
            char messageId[37];
            nextMessageId(messageId);
            const size_t envelope = nivaloBuildEnvelope(_deviceId.c_str(), messageId, _clock.read(monotonicUs()).utcMs,
                                                        NULL, NULL, payload, length, _envelope, sizeof(_envelope));
            if (envelope == 0U)
            {
                _droppedBatches++;
            }
            else if (_connection.connected() && _connection.publish(_topics.telemetry(), _envelope, false))
            {
                published = true;
            }
            else
            {
                bufferTelemetry(_envelope, envelope);
            }
        }
    }
    _batch.reset();
    _newSample = true;
    return published;
}

bool NivaloDevice::openSample()
{
    if (!clockValid())
    {
        return false;
    }
    if (!_newSample && !_batch.empty())
    {
        return true;
    }
    const int64_t now = _clock.read(monotonicUs()).utcMs;
    if (!_batch.beginSample(now))
    {
        // Eight samples already, or a second sample in the same millisecond: start a new batch.
        publishBatch();
        if (!_batch.beginSample(now))
        {
            return false;
        }
    }
    _newSample = false;
    return true;
}

bool NivaloDevice::addTelemetry(const char *name, double value, const char *unit)
{
    if (!openSample())
    {
        return false;
    }
    if (_batch.addValue(name, value, unit))
    {
        return true;
    }
    // The batch is full (readings or bytes): publish it and continue in a fresh one.
    publishBatch();
    return openSample() && _batch.addValue(name, value, unit);
}

bool NivaloDevice::addTelemetryRaw(const char *name, const char *raw, const char *unit)
{
    if (!openSample())
    {
        return false;
    }
    if (_batch.addRaw(name, raw, unit))
    {
        return true;
    }
    publishBatch();
    return openSample() && _batch.addRaw(name, raw, unit);
}

void NivaloDevice::nextTelemetrySample()
{
    _newSample = true;
}

size_t NivaloDevice::flushTelemetry()
{
    return _batch.empty() ? 0U : (publishBatch() ? 1U : 0U);
}

size_t NivaloDevice::publishTelemetry(const char *name, const char *value, const char *unit)
{
    flushTelemetry();
    double number = 0.0;
    const bool added = value != NULL && parseNumber(value, &number) ? addTelemetry(name, number, unit)
                                                                    : addTelemetryRaw(name, value == NULL ? "" : value, unit);
    if (!added)
    {
        _batch.reset();
        _newSample = true;
        return 0U;
    }
    return publishBatch() ? 1U : 0U;
}

size_t NivaloDevice::publishVariables()
{
    flushTelemetry();
    for (size_t i = 0U; i < _sdkRegistry.variableCount(); i++)
    {
        const NivaloRegisteredVariable &variable = _sdkRegistry.variableAt(i);
        double number = 0.0;
        String raw;
        if (!_sdkRegistry.readVariable(i, &number, &raw))
        {
            continue;
        }
        const char *unit = variable.unit.length() > 0U ? variable.unit.c_str() : NULL;
        const bool numeric = _sdkRegistry.variableDataType(i)[0] == 'n';
        if (!(numeric ? addTelemetry(variable.name.c_str(), number, unit)
                      : addTelemetryRaw(variable.name.c_str(), raw.c_str(), unit)))
        {
            Serial.print("Nivalo: variable not reportable: ");
            Serial.println(variable.name);
        }
    }
    return flushTelemetry();
}

size_t NivaloDevice::publishRuntimeTelemetry()
{
    flushTelemetry();
    const uint32_t heapTotal = ESP.getHeapSize();
    const uint32_t heapFree = min((uint32_t)ESP.getFreeHeap(), heapTotal);
    addTelemetry("uptime_s", (double)(monotonicUs() / 1000000LL), "s");
    if (WiFi.status() == WL_CONNECTED)
    {
        addTelemetry("wifi_rssi", WiFi.RSSI(), "dBm");
    }
    addTelemetry("heap_used", heapTotal - heapFree, "B");
    addTelemetry("heap_free", heapFree, "B");
    addTelemetry("heap_total", heapTotal, "B");
    return flushTelemetry();
}

// --- Events and state ----------------------------------------------------------------------------

size_t NivaloDevice::publish(const char *eventName, const char *eventData)
{
    return publishEvent(eventName, eventData);
}

size_t NivaloDevice::publishEvent(const char *name, const char *data, const char *severity)
{
    char *payload = _payload;
    const bool structured = data != NULL && nivaloScanJson(data, strlen(data), NivaloV11::MaxDepth - 2U,
                                                           NivaloV11::MaxValues, NULL) == NIVALO_JSON_OK;
    const size_t length = nivaloEventPayload(name, severity == NULL ? "info" : severity, structured ? data : NULL,
                                             structured ? NULL : data, payload, sizeof(_payload));
    return length > 0U && publishEnvelope(_topics.events(), payload, length, NULL, NULL, false) ? 1U : 0U;
}

size_t NivaloDevice::publishState(const char *name, const char *valueJson)
{
    const size_t length = nivaloStatePayload(name, valueJson, _payload, sizeof(_payload));
    return length > 0U && publishEnvelope(_topics.state(), _payload, length, NULL, NULL, false) ? 1U : 0U;
}
