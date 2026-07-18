#include "NivaloDevice.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <stdlib.h>

namespace
{
void addFirmwareTargets(JsonVariant firmware)
{
    JsonArray targets = firmware["targets"].to<JsonArray>();
    targets.add("esp32");
#if NIVALO_HAS_SECONDARY_MCU
    targets.add("stm32");
#endif
}
} // namespace

void NivaloDevice::mqttReconnect()
{
    _connection.service(millis());
}

void NivaloDevice::serviceConnection()
{
    if (!_protocol.timeValid())
    {
        return;
    }
    if (!_lastWillConfigured)
    {
        StaticJsonDocument<512> will;
        will["schema"] = "nivalo.iot.v1";
        will["messageId"] = createMessageId();
        will["deviceId"] = _deviceId;
        will["sentAt"] = createTimestamp();
        will["payload"]["status"] = "offline";
        will["payload"]["reason"] = "mqtt-disconnected";
        String payload;
        serializeJson(will, payload);
        _connection.setLastWill(payload);
        _lastWillConfigured = true;
    }
    _connection.service(millis());
    if (_connection.takeJustConnected())
    {
        _statusPixel.set(52, 204, 235);
        publishAvailability("online", "mqtt-connected");
        publishDefinitions();
        publishVariables();
        flushTelemetryBuffer();
    }
}

void NivaloDevice::flushTelemetryBuffer()
{
    while (_connection.connected() && _telemetryBufferCount > 0U)
    {
        BufferedTelemetry &entry = _telemetryBuffer[_telemetryBufferHead];
        if (!_connection.publish(_telemetryTopic, entry.payload.c_str()))
        {
            break;
        }
        entry.payload = "";
        _telemetryBufferHead = (_telemetryBufferHead + 1U) % 8U;
        _telemetryBufferCount--;
    }
}

size_t NivaloDevice::publish(const char *eventName, const char *eventData)
{
    return publishEvent(eventName, eventData);
}

size_t NivaloDevice::publishTelemetry(const char *name, const char *value, const char *unit)
{
    if (!_protocol.timeValid())
    {
        return 0U;
    }
    String output;
    StaticJsonDocument<768> doc;
    doc["schema"] = "nivalo.iot.v1";
    doc["messageId"] = createMessageId();
    doc["deviceId"] = _deviceId;
    doc["sentAt"] = createTimestamp();
    doc["firmware"]["version"] = _firmwareVersion;
    doc["firmware"]["hardware"] = _hardwareName;
    doc["firmware"]["mac"] = _macAddr;
    addFirmwareTargets(doc["firmware"]);
    doc["payload"]["name"] = name;

    if (value == NULL)
    {
        doc["payload"]["raw"] = "";
    }
    else
    {
        char *end = NULL;
        double numericValue = strtod(value, &end);
        if (end != value && *end == '\0')
        {
            doc["payload"]["value"] = numericValue;
        }
        else
        {
            doc["payload"]["raw"] = value;
        }
    }

    if (unit != NULL)
    {
        doc["payload"]["unit"] = unit;
    }

    serializeJson(doc, output);

    if (_connection.publish(_telemetryTopic, output.c_str()))
    {
        return 1U;
    }
    if (_telemetryBufferCapacity > 0U)
    {
        if (_telemetryBufferCount == _telemetryBufferCapacity)
        {
            _telemetryBuffer[_telemetryBufferHead].payload = "";
            _telemetryBufferHead = (_telemetryBufferHead + 1U) % 8U;
            _telemetryBufferCount--;
        }
        uint8_t tail = (_telemetryBufferHead + _telemetryBufferCount) % 8U;
        _telemetryBuffer[tail].payload = output;
        _telemetryBufferCount++;
    }
    return 0U;
}

size_t NivaloDevice::publishRuntimeTelemetry()
{
    size_t published = 0;
    published += publishUptimeTelemetry();
    published += publishWifiSignalTelemetry();
    published += publishHeapTelemetry();
    return published;
}

size_t NivaloDevice::publishUptimeTelemetry()
{
    String uptime = String(millis());
    return publishTelemetry("uptime_ms", uptime.c_str(), "ms");
}

size_t NivaloDevice::publishWifiSignalTelemetry()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return 0;
    }

    String rssi = String(WiFi.RSSI());
    return publishTelemetry("wifi_rssi", rssi.c_str(), "dBm");
}

size_t NivaloDevice::publishHeapTelemetry()
{
    uint32_t heapTotal = ESP.getHeapSize();
    uint32_t heapFree = ESP.getFreeHeap();
    uint32_t heapUsed = heapTotal > heapFree ? heapTotal - heapFree : 0;

    String heapUsedText = String(heapUsed);
    String heapFreeText = String(heapFree);
    String heapTotalText = String(heapTotal);

    size_t published = 0;
    published += publishTelemetry("heap_used", heapUsedText.c_str(), "B");
    published += publishTelemetry("heap_free", heapFreeText.c_str(), "B");
    published += publishTelemetry("heap_total", heapTotalText.c_str(), "B");
    return published;
}

size_t NivaloDevice::publishEvent(const char *name, const char *data, const char *severity)
{
    if (!_protocol.timeValid())
    {
        return 0U;
    }
    String output;
    StaticJsonDocument<1536> doc;
    doc["schema"] = "nivalo.iot.v1";
    doc["messageId"] = createMessageId();
    doc["deviceId"] = _deviceId;
    doc["sentAt"] = createTimestamp();
    doc["firmware"]["version"] = _firmwareVersion;
    doc["firmware"]["hardware"] = _hardwareName;
    addFirmwareTargets(doc["firmware"]);
    doc["payload"]["name"] = name;
    doc["payload"]["severity"] = severity;
    StaticJsonDocument<1024> parsedData;
    const char *eventDataJson = data;
    bool structuredData = eventDataJson != NULL && !deserializeJson(parsedData, eventDataJson);
    if (structuredData)
    {
        // Preserve structured device events as canonical data. The API keeps
        // this field for every event while raw envelopes are sampled.
        doc["payload"]["data"] = serialized(eventDataJson);
    }
    else
    {
        doc["payload"]["raw"] = data;
    }
    serializeJson(doc, output);

    return _connection.publish(_eventsTopic, output.c_str()) ? 1U : 0U;
}

size_t NivaloDevice::publishAvailability(const char *status, const char *reason)
{
    if (!_protocol.timeValid())
    {
        return 0U;
    }
    String output;
    StaticJsonDocument<512> doc;
    doc["schema"] = "nivalo.iot.v1";
    doc["messageId"] = createMessageId();
    doc["deviceId"] = _deviceId;
    doc["sentAt"] = createTimestamp();
    doc["payload"]["status"] = status;
    if (reason != NULL)
    {
        doc["payload"]["reason"] = reason;
    }
    serializeJson(doc, output);

    return _connection.publish(_availabilityTopic, output.c_str(), true) ? 1U : 0U;
}

