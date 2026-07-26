#include "NivaloDevice.h"

#include <ArduinoJson.h>

void NivaloDevice::mqttCallback(char *topic, byte *message, unsigned int length)
{
    Serial.print("Message arrived on topic: ");
    Serial.println(topic);
    DynamicJsonDocument doc(2048);
    // PubSubClient owns and reuses `message`. Treat it as read-only so
    // ArduinoJson copies strings into `doc`; otherwise status publishes made
    // later in this callback can overwrite zero-copy command arguments.
    const char *commandJson = reinterpret_cast<const char *>(message);
    DeserializationError err = deserializeJson(doc, commandJson, length);
    if (err)
    {
        Serial.print("MQTT command JSON parse failed: ");
        Serial.println(err.c_str());
        queueEventReport("esp32.command.parse_error", err.c_str(), "warning");
        return;
    }

    String commandId = getCommandIdFromTopic(topic);
    String commandName;
    if (doc["payload"]["command"].is<const char *>())
    {
        commandName = doc["payload"]["command"].as<String>();
    }
    else if (doc["command"].is<const char *>())
    {
        commandName = doc["command"].as<String>();
    }
    String payload;
    JsonVariant arguments = doc["payload"]["arguments"];
    const char *requestedBy = doc["payload"]["requestedBy"].is<const char *>()
                                  ? doc["payload"]["requestedBy"].as<const char *>()
                                  : NULL;
    if (requestedBy == NULL && doc["requestedBy"].is<const char *>())
    {
        requestedBy = doc["requestedBy"].as<const char *>();
    }
    if (arguments.is<const char *>())
    {
        payload = arguments.as<String>();
    }
    else if (arguments["url"].is<const char *>())
    {
        payload = arguments["url"].as<String>();
    }
    else if (arguments["signedUrl"].is<const char *>())
    {
        payload = arguments["signedUrl"].as<String>();
    }
    else if (arguments["downloadUrl"].is<const char *>())
    {
        payload = arguments["downloadUrl"].as<String>();
    }
    else if (!arguments.isNull())
    {
        serializeJson(arguments, payload);
    }
    else if (doc["payload"].is<const char *>())
    {
        payload = doc["payload"].as<String>();
    }

    Serial.println("-----------------------------");
    Serial.println("MQTT command received");
    Serial.print("command: ");
    Serial.println(commandName);
    if (commandId.length() > 0U)
    {
        Serial.print("commandId: ");
        Serial.println(commandId);
    }
    if (arguments["target"].is<const char *>())
    {
        Serial.print("target: ");
        Serial.println(arguments["target"].as<const char *>());
    }
    Serial.println("-----------------------------");
    queueEventReport("esp32.command.received", commandName.c_str());

    if (commandId.length() > 0)
    {
        queueCommandAckReport(commandId.c_str(), "accepted", "Command received by ESP32");
    }
    drainQueuedReports();
    _connection.client().loop();

    if (commandName == "flash" || commandName == "firmware.flash")
    {
        handleFirmwareCommand(commandId, arguments);
    }
    else if (commandName.length() > 0 && dispatchSdkCommand(commandId.c_str(), commandName, payload))
    {
        // Locally registered handlers own their full terminal ACK lifecycle.
    }
    else if (commandName.length() > 0)
    {
#if NIVALO_HAS_SECONDARY_MCU
        StaticJsonDocument<512> stmCommand;
        stmCommand["commandId"] = commandId;
        stmCommand["command"] = commandName;
        if (requestedBy != NULL && strlen(requestedBy) > 0U)
        {
            stmCommand["requestedBy"] = requestedBy;
        }
        String forwardedCommand;
        serializeJson(stmCommand, forwardedCommand);
        if (forwardedCommand.endsWith("}"))
        {
            forwardedCommand.remove(forwardedCommand.length() - 1U);
        }
        forwardedCommand += ",\"payload\":";
        if (!arguments.isNull())
        {
            // Serialize the source value straight into the final frame. Moving
            // a nested value between ArduinoJson documents can preserve only
            // an empty container on the ESP32 toolchain used by this bridge.
            serializeJson(arguments, forwardedCommand);
        }
        else if (doc["payload"].is<const char *>())
        {
            serializeJson(doc["payload"], forwardedCommand);
        }
        else
        {
            forwardedCommand += "{}";
        }
        forwardedCommand += "}";
        if (stmCommand.overflowed() || forwardedCommand.length() > NIVALO_LINK_MAX_PAYLOAD)
        {
            queueEventReport("mcu.command.forward_failed", "command JSON exceeds bridge capacity", "warning");
            if (commandId.length() > 0)
            {
                queueCommandAckReport(commandId.c_str(), "failed", "Command JSON exceeds bridge capacity");
            }
            return;
        }
        NivaloLinkReceivedFrame immediateFrame;
        bool sent = _link.transport().exchange(NIVALO_LINK_FRAME_COMMAND, forwardedCommand.c_str(), &immediateFrame);

        if (sent)
        {
            _link.noteCommandExchange(millis());
            queueEventReport("mcu.command.forwarded", forwardedCommand.c_str());
            if (commandId.length() > 0)
            {
                queueCommandAckReport(commandId.c_str(), "running", "Forwarded to secondary MCU over NivaloLink");
            }
            if (immediateFrame.valid)
            {
                handleNivaloLinkFrame(immediateFrame);
            }
        }
        else
        {
            queueEventReport("mcu.command.forward_failed", _link.transport().lastError(), "warning");
            if (commandId.length() > 0)
            {
                queueCommandAckReport(commandId.c_str(), "failed", _link.transport().lastError());
            }
        }
#else
        queueEventReport("esp32.command.unsupported", commandName.c_str(), "warning");
        if (commandId.length() > 0)
        {
            queueCommandAckReport(commandId.c_str(), "failed", "No secondary MCU bridge is enabled");
        }
#endif
    }

    Serial.println();
}
