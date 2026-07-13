#include "NivaloDevice.h"

#include <ArduinoJson.h>

namespace
{
static constexpr size_t PendingReportCount = 8U;
static void copyText(char *destination, size_t destinationSize, const char *source)
{
    if (destination == NULL || destinationSize == 0)
    {
        return;
    }

    if (source == NULL)
    {
        source = "";
    }

    strncpy(destination, source, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

}

void NivaloDevice::drainQueuedReports()
{
    for (size_t i = 0; i < PendingReportCount; i++)
    {
        if (_pendingCommandAcks[i].queued)
        {
            PendingCommandAckState ack = _pendingCommandAcks[i];
            _pendingCommandAcks[i].queued = false;
            publishCommandAck(ack.commandId, ack.status, ack.message, ack.progress);
        }
    }

    for (size_t i = 0; i < PendingReportCount; i++)
    {
        if (_pendingEvents[i].queued)
        {
            PendingEventState event = _pendingEvents[i];
            _pendingEvents[i].queued = false;
            publishEvent(event.name, event.data, event.severity);
        }
    }
}

void NivaloDevice::publishCommandAck(const char *commandId, const char *status, const char *message, float progress)
{
    if (commandId == NULL || strlen(commandId) == 0)
    {
        return;
    }

    char canonicalTopic[200];
    snprintf(canonicalTopic, sizeof(canonicalTopic), "%s%s/ack", _commandAckTopicPrefix, commandId);

    String output;
    StaticJsonDocument<512> doc;
    doc["schema"] = "nivalo.iot.v1";
    doc["messageId"] = createMessageId();
    doc["correlationId"] = commandId;
    doc["deviceId"] = _deviceId;
    doc["sentAt"] = createTimestamp();
    doc["payload"]["status"] = status;
    if (message != NULL)
    {
        doc["payload"]["message"] = message;
    }
    if (progress >= 0)
    {
        doc["payload"]["progress"] = progress;
    }

    serializeJson(doc, output);
    if (!_connection.connected())
    {
        mqttReconnect();
    }

    bool canonicalPublished = _connection.publish(canonicalTopic, output.c_str());
    if (!canonicalPublished)
    {
        Serial.print("Canonical command ACK publish failed: ");
        Serial.println(commandId);
    }

#if NIVALO_PUBLISH_LEGACY_COMMAND_ACKS
    char legacyTopic[190];
    snprintf(legacyTopic, sizeof(legacyTopic), "%s%s", _legacyAckTopicPrefix, commandId);
    bool legacyPublished = _connection.publish(legacyTopic, output.c_str());
    if (!legacyPublished)
    {
        Serial.print("Legacy command ACK publish failed: ");
        Serial.println(commandId);
    }

    if (canonicalPublished || legacyPublished)
#else
    if (canonicalPublished)
#endif
    {
        _connection.client().loop();
    }
}

String NivaloDevice::getCommandIdFromTopic(const char *topic)
{
    String value = String(topic);
    int index = value.lastIndexOf('/');
    if (index < 0 || index + 1 >= value.length())
    {
        return "";
    }

    return value.substring(index + 1);
}

String NivaloDevice::createMessageId()
{
    return _protocol.messageId();
}

String NivaloDevice::createTimestamp()
{
    return _protocol.timestamp();
}

bool NivaloDevice::queueCommandAckReport(const char *commandId, const char *status, const char *message, float progress)
{
    if (commandId == NULL || strlen(commandId) == 0)
    {
        return false;
    }

    for (size_t i = 0; i < PendingReportCount; i++)
    {
        if (!_pendingCommandAcks[i].queued)
        {
            copyText(_pendingCommandAcks[i].commandId, sizeof(_pendingCommandAcks[i].commandId), commandId);
            copyText(_pendingCommandAcks[i].status, sizeof(_pendingCommandAcks[i].status), status);
            copyText(_pendingCommandAcks[i].message, sizeof(_pendingCommandAcks[i].message), message);
            _pendingCommandAcks[i].progress = progress;
            _pendingCommandAcks[i].queued = true;
            return true;
        }
    }

    return false;
}

bool NivaloDevice::queueEventReport(const char *name, const char *data, const char *severity)
{
    for (size_t i = 0; i < PendingReportCount; i++)
    {
        if (!_pendingEvents[i].queued)
        {
            copyText(_pendingEvents[i].name, sizeof(_pendingEvents[i].name), name);
            copyText(_pendingEvents[i].data, sizeof(_pendingEvents[i].data), data);
            copyText(_pendingEvents[i].severity, sizeof(_pendingEvents[i].severity), severity);
            _pendingEvents[i].queued = true;
            return true;
        }
    }

    return false;
}

