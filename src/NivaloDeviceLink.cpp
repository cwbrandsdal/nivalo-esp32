#include "NivaloDevice.h"

#include <ArduinoJson.h>
#include <WiFi.h>

namespace
{
static constexpr size_t UartMessageJsonCapacity = 2048U;
static constexpr size_t DefinitionsJsonCapacity = 3072U;
static constexpr unsigned long HelloMs = 30000UL;
static constexpr unsigned long HeartbeatMs = 30000UL;
static constexpr unsigned long InvalidFrameReportMs = 60000UL;
static constexpr unsigned long InvalidFrameBackoffMs = 250UL;
static constexpr size_t DrainLimit = 8U;

void addFirmwareTargets(JsonVariant firmware)
{
    JsonArray targets = firmware["targets"].to<JsonArray>();
    targets.add("esp32");
#if NIVALO_HAS_SECONDARY_MCU
    targets.add("stm32");
#endif
}

const char *wifiStatusName(int status)
{
    switch (status)
    {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "no_ssid";
    case WL_SCAN_COMPLETED: return "scan_completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect_failed";
    case WL_CONNECTION_LOST: return "connection_lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
    }
}
} // namespace

void NivaloDevice::publishNivaloLinkHeartbeat(const char *payload)
{
    DynamicJsonDocument doc(1536);
    bool parsedPayload = false;

    if (payload != NULL && strlen(payload) > 0U)
    {
        DeserializationError err = deserializeJson(doc, payload);
        parsedPayload = (err == DeserializationError::Ok && doc.is<JsonObject>());
    }

    if (!parsedPayload)
    {
        doc.clear();
        doc.to<JsonObject>();
    }

    JsonObject esp32 = doc.createNestedObject("esp32");
    esp32["uptimeMs"] = millis();
    esp32["firmwareVersion"] = _firmwareVersion;
    esp32["hardware"] = _hardwareName;
    esp32["mac"] = _macAddr;
    esp32["sdkVersion"] = ESP.getSdkVersion();
    esp32["chipModel"] = ESP.getChipModel();
    esp32["chipRevision"] = ESP.getChipRevision();
    esp32["cpuFreqMHz"] = ESP.getCpuFreqMHz();

    JsonObject wifi = esp32.createNestedObject("wifi");
    int wifiStatus = WiFi.status();
    wifi["status"] = wifiStatusName(wifiStatus);
    wifi["statusCode"] = wifiStatus;
    wifi["connected"] = (wifiStatus == WL_CONNECTED);
    if (wifiStatus == WL_CONNECTED)
    {
        wifi["rssiDbm"] = WiFi.RSSI();
        wifi["channel"] = WiFi.channel();
        wifi["ip"] = WiFi.localIP().toString();
    }

    JsonObject heap = esp32.createNestedObject("heap");
    heap["totalBytes"] = ESP.getHeapSize();
    heap["freeBytes"] = ESP.getFreeHeap();
    heap["minFreeBytes"] = ESP.getMinFreeHeap();
    heap["maxAllocBytes"] = ESP.getMaxAllocHeap();

    JsonObject sketch = esp32.createNestedObject("sketch");
    sketch["sizeBytes"] = ESP.getSketchSize();
    sketch["freeBytes"] = ESP.getFreeSketchSpace();

    String enrichedPayload;
    if (!doc.overflowed() && serializeJson(doc, enrichedPayload) > 0U)
    {
        publishEvent("mcu.nivalolink.heartbeat", enrichedPayload.c_str(), "info");
        return;
    }

    publishEvent("mcu.nivalolink.heartbeat", payload != NULL ? payload : "{}", "info");
}

void NivaloDevice::handleNivaloLinkFrame(const NivaloLinkReceivedFrame &frame)
{
    if (!frame.valid || frame.duplicate || frame.type == NIVALO_LINK_FRAME_IDLE || frame.type == NIVALO_LINK_FRAME_POLL)
    {
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_HELLO)
    {
        publishEvent("mcu.nivalolink.hello", frame.payloadLength > 0U ? frame.payload : "{}", "info");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_HEARTBEAT)
    {
        publishNivaloLinkHeartbeat(frame.payloadLength > 0U ? frame.payload : "{}");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_ERROR)
    {
        publishEvent("mcu.nivalolink.error", frame.payloadLength > 0U ? frame.payload : "{}", "warning");
        return;
    }

    DynamicJsonDocument doc(UartMessageJsonCapacity);
    DeserializationError err = deserializeJson(doc, frame.payload);
    if (err)
    {
        char parseMessage[96];
        snprintf(
            parseMessage,
            sizeof(parseMessage),
            "%s type=0x%02x bytes=%u",
            err.c_str(),
            frame.type,
            (unsigned int)frame.payloadLength);
        publishEvent("esp32.nivalolink.parse_error", parseMessage, "warning");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_TELEMETRY)
    {
        String value;
        JsonVariant payloadValue = doc["value"];
        const char *name = doc["name"] | "";
        const char *unit = doc["unit"] | "";
        if (strlen(unit) == 0)
        {
            unit = NULL;
        }

        if (strlen(name) == 0 || payloadValue.isNull())
        {
            publishEvent("mcu.telemetry.rejected", frame.payload, "warning");
            return;
        }

        if (payloadValue.is<const char *>())
        {
            value = payloadValue.as<const char *>();
        }
        else
        {
            serializeJson(payloadValue, value);
        }

        publishTelemetry(name, value.c_str(), unit);
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_EVENT)
    {
        const char *eventName = doc["eventName"].is<const char *>() ? doc["eventName"].as<const char *>() : NULL;
        if (eventName == NULL)
        {
            eventName = doc["name"] | "mcu.event";
        }
        const char *eventData = doc["eventData"].is<const char *>() ? doc["eventData"].as<const char *>() : NULL;
        if (eventData == NULL)
        {
            eventData = doc["data"].is<const char *>() ? doc["data"].as<const char *>() : NULL;
        }
        if (eventData == NULL)
        {
            eventData = doc["raw"] | frame.payload;
        }
        const char *severity = doc["severity"] | "info";
        publishEvent(eventName, eventData, severity);
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_DEFINITION)
    {
        JsonVariant payload = doc.as<JsonVariant>();
        if (!doc["payload"].isNull())
        {
            payload = doc["payload"];
        }

        JsonArray variables = payload["variables"].as<JsonArray>();
        JsonArray functions = payload["functions"].as<JsonArray>();
        size_t variableCount = variables.isNull() ? 0 : variables.size();
        size_t functionCount = functions.isNull() ? 0 : functions.size();

        DynamicJsonDocument definitions(DefinitionsJsonCapacity);
        definitions["schema"] = "nivalo.iot.v1";
        definitions["messageId"] = createMessageId();
        definitions["deviceId"] = _deviceId;
        definitions["sentAt"] = createTimestamp();
        definitions["firmware"]["version"] = _firmwareVersion;
        definitions["firmware"]["hardware"] = _hardwareName;
        addFirmwareTargets(definitions["firmware"]);
        JsonObject definitionsPayload = definitions["payload"].to<JsonObject>();
        appendSdkDefinitions(definitionsPayload);
        if (!variables.isNull())
        {
            JsonArray mergedVariables = definitionsPayload["variables"].as<JsonArray>();
            if (mergedVariables.isNull())
            {
                mergedVariables = definitionsPayload["variables"].to<JsonArray>();
            }
            for (JsonVariant variable : variables)
            {
                mergedVariables.add(variable);
            }
        }
        if (!functions.isNull())
        {
            JsonArray mergedFunctions = definitionsPayload["functions"].as<JsonArray>();
            if (mergedFunctions.isNull())
            {
                mergedFunctions = definitionsPayload["functions"].to<JsonArray>();
            }
            for (JsonVariant function : functions)
            {
                mergedFunctions.add(function);
            }
        }

        String definitionsOutput;
        size_t definitionsBytes = serializeJson(definitions, definitionsOutput);
        bool publishOk = false;
        if (!definitions.overflowed())
        {
            publishOk = _connection.publish(_definitionsTopic, definitionsOutput.c_str());
        }

        char publishMessage[160];
        snprintf(
            publishMessage,
            sizeof(publishMessage),
            "variables=%u functions=%u bytes=%u overflow=%s mqtt=%s",
            (unsigned int)variableCount,
            (unsigned int)functionCount,
            (unsigned int)definitionsBytes,
            definitions.overflowed() ? "true" : "false",
            publishOk ? "published" : "failed");
        publishEvent(
            publishOk ? "esp32.definitions.publish.ok" : "esp32.definitions.publish.failed",
            publishMessage,
            publishOk ? "info" : "warning");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_DEFINITIONS_BEGIN)
    {
        publishEvent("mcu.definitions.begin", frame.payloadLength > 0U ? frame.payload : "{}", "info");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_DEFINITIONS_END)
    {
        publishEvent("mcu.definitions.end", frame.payloadLength > 0U ? frame.payload : "{}", "info");
        return;
    }

    if (frame.type == NIVALO_LINK_FRAME_COMMAND_STATUS)
    {
        const char *commandId = doc["commandId"] | "";
        const char *status = doc["status"] | "failed";
        const char *message = doc["message"].is<const char *>() ? doc["message"].as<const char *>() : NULL;
        float progress = -1;
        if (!doc["progress"].isNull())
        {
            progress = doc["progress"].as<float>();
        }

        publishCommandAck(commandId, status, message, progress);
        return;
    }

    char eventName[48];
    snprintf(eventName, sizeof(eventName), "mcu.nivalolink.type_0x%02x", frame.type);
    publishEvent(eventName, frame.payloadLength > 0U ? frame.payload : "{}", "info");
}

void NivaloDevice::drainNivaloLink()
{
    if (_link.transport().isPaused())
    {
        return;
    }

    unsigned long now = millis();
    bool helloDue = _link.helloPending || (now - _link.lastHello) >= HelloMs;
    bool heartbeatDue = (now - _link.lastHeartbeat) >= HeartbeatMs;
    bool pollDue =
        (now - _link.lastPoll) >= NivaloLinkCommandPollPolicy::FallbackPollMs;
    bool dataReady = _link.transport().dataReady();
    if (_link.shouldDeferCommandPoll(now, dataReady))
    {
        return;
    }
    if (!heartbeatDue && now < _link.backoffUntil)
    {
        return;
    }

    bool shouldDrain = dataReady || helloDue || heartbeatDue || pollDue;
    if (!shouldDrain)
    {
        return;
    }

    size_t drained = 0U;
    do
    {
        NivaloLinkReceivedFrame frame;
        bool sent;
        _link.notePollStarted();
        if (helloDue)
        {
            StaticJsonDocument<160> hello;
            hello["protocol"] = "NivaloLink";
            hello["version"] = 1;
            hello["frameSize"] = NIVALO_LINK_SPI_FRAME_SIZE;
            hello["transport"] = "spi-master";
            String payload;
            serializeJson(hello, payload);
            sent = _link.transport().exchange(NIVALO_LINK_FRAME_HELLO, payload.c_str(), &frame);
            if (sent)
            {
                _link.helloPending = false;
                _link.lastHello = now;
                helloDue = false;
            }
        }
        else if (heartbeatDue)
        {
            StaticJsonDocument<128> heartbeat;
            heartbeat["uptimeMs"] = millis();
            String payload;
            serializeJson(heartbeat, payload);
            sent = _link.transport().exchange(NIVALO_LINK_FRAME_HEARTBEAT, payload.c_str(), &frame);
            _link.lastHeartbeat = now;
            heartbeatDue = false;
        }
        else
        {
            sent = _link.transport().poll(&frame);
        }

        _link.lastPoll = now;
        if (!sent)
        {
            queueEventReport("esp32.nivalolink.poll_failed", _link.transport().lastError(), "warning");
            return;
        }

        if (frame.valid)
        {
            handleNivaloLinkFrame(frame);
            if (frame.type == NIVALO_LINK_FRAME_IDLE && !_link.transport().dataReady())
            {
                break;
            }
        }
        else
        {
            _link.invalidFrameCount++;
            _link.backoffUntil = millis() + InvalidFrameBackoffMs;

            unsigned long reportNow = millis();
            bool firstInvalidFrameReport = _link.lastInvalidFrameReport == 0U;
            if (firstInvalidFrameReport || (reportNow - _link.lastInvalidFrameReport) >= InvalidFrameReportMs)
            {
                char rxPrefix[25];
                char diagnosticJson[256];
                _link.transport().formatLastRxPrefix(rxPrefix, sizeof(rxPrefix), 12U);
                StaticJsonDocument<256> diagnostic;
                diagnostic["error"] = _link.transport().lastError();
                diagnostic["count"] = _link.invalidFrameCount;
                diagnostic["dataReady"] = _link.transport().dataReady();
                diagnostic["rxPrefix"] = rxPrefix;
                serializeJson(diagnostic, diagnosticJson, sizeof(diagnosticJson));

                if (queueEventReport("esp32.nivalolink.frame_invalid", diagnosticJson, "warning"))
                {
                    _link.lastInvalidFrameReport = reportNow;
                    _link.invalidFrameCount = 0;
                }
            }
            break;
        }

        drained++;
    } while (_link.transport().dataReady() && drained < DrainLimit);
}
