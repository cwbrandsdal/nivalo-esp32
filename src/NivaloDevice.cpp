#include <Arduino.h>
#include "NivaloDevice.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

static constexpr size_t UART_MESSAGE_JSON_CAPACITY = 2048;
static constexpr size_t DEFINITIONS_JSON_CAPACITY = 3072;

static void addFirmwareTargets(JsonVariant firmware)
{
    JsonArray targets = firmware["targets"].to<JsonArray>();
    targets.add("esp32");
#if NIVALO_HAS_SECONDARY_MCU
    targets.add("stm32");
#endif
}

void NivaloDevice::loop()
{
    if (_watchdogEnabled)
    {
        esp_task_wdt_reset();
    }
    _statusPixel.pulse();
    serviceConnection();
    if (!_connection.connected())
    {
        _statusPixel.set(255, 0, 0);
    }

    drainQueuedReports();
#if NIVALO_HAS_SECONDARY_MCU
    drainNivaloLink();
#endif

    if (millis() - _lastHeartbeat > 60000)
    {
        _lastHeartbeat = millis();

        publishAvailability("online", "heartbeat");
        publishRuntimeTelemetry();
    }

    while (hwAvailable() > 0)
    {
        String uartLine = _hardSerial->readStringUntil('\n');
        uartLine.trim();
        if (uartLine.length() == 0)
        {
            continue;
        }

        Serial.println("GETTING DATA!");
        // Allocate the JSON document
        // This one must be bigger than for the sender because it must store the strings
        DynamicJsonDocument doc(UART_MESSAGE_JSON_CAPACITY);

        // Read one complete newline-delimited JSON document from the STM32 link.
        DeserializationError err = deserializeJson(doc, uartLine);

        if (err == DeserializationError::Ok)
        {
            String commandName = doc["command"].as<String>();
            Serial.println("COMMAND:");
            Serial.println(commandName);
            const char *payload = doc["payload"];

            if (commandName == "function")
            {
            }
            else if (commandName == "publish")
            {
                String output;
                serializeJson(doc, output);

                const char *eventName = doc["payload"]["eventName"] | "stm32.publish";
                const char *eventData = doc["payload"]["eventData"] | output.c_str();
                publishEvent(eventName, eventData);
            }
            else if (commandName == "variable" || commandName == "telemetry")
            {
                String value;
                JsonVariant payload = doc["payload"];
                JsonVariant payloadValue = payload["value"];
                const char *name = payload["name"] | "";
                if (strlen(name) == 0)
                {
                    name = payload["variable"] | "";
                }
                const char *unit = payload["unit"] | "";
                if (strlen(unit) == 0)
                {
                    unit = NULL;
                }

                if (strlen(name) == 0 || payloadValue.isNull())
                {
                    String output;
                    serializeJson(doc, output);
                    publishEvent("stm32.variable.rejected", output.c_str(), "warning");
                }
                else
                {
                    if (payloadValue.is<const char *>())
                    {
                        value = payloadValue.as<const char *>();
                    }
                    else
                    {
                        serializeJson(payloadValue, value);
                    }

                    publishTelemetry(name, value.c_str(), unit);
                }
            }
            else if (commandName == "register")
            {
                String output;
                serializeJson(doc, output);

                JsonArray variables = doc["payload"]["variables"].as<JsonArray>();
                JsonArray functions = doc["payload"]["functions"].as<JsonArray>();
                size_t variableCount = variables.isNull() ? 0 : variables.size();
                size_t functionCount = functions.isNull() ? 0 : functions.size();
                char parsedMessage[120];
                snprintf(
                    parsedMessage,
                    sizeof(parsedMessage),
                    "variables=%u functions=%u bytes=%u",
                    (unsigned int)variableCount,
                    (unsigned int)functionCount,
                    (unsigned int)output.length());
                publishEvent("esp32.register.parsed", parsedMessage);

                DynamicJsonDocument definitions(DEFINITIONS_JSON_CAPACITY);
                definitions["schema"] = "nivalo.iot.v1";
                definitions["messageId"] = createMessageId();
                definitions["deviceId"] = _deviceId;
                definitions["sentAt"] = createTimestamp();
                definitions["firmware"]["version"] = _firmwareVersion;
                definitions["firmware"]["hardware"] = _hardwareName;
                addFirmwareTargets(definitions["firmware"]);
                JsonObject definitionsPayload = definitions["payload"].to<JsonObject>();
                appendSdkDefinitions(definitionsPayload);
                if (!doc["payload"]["variables"].isNull())
                {
                    JsonArray mergedVariables = definitionsPayload["variables"].as<JsonArray>();
                    if (mergedVariables.isNull())
                    {
                        mergedVariables = definitionsPayload["variables"].to<JsonArray>();
                    }
                    for (JsonVariant variable : doc["payload"]["variables"].as<JsonArray>())
                    {
                        mergedVariables.add(variable);
                    }
                }
                if (!doc["payload"]["functions"].isNull())
                {
                    JsonArray mergedFunctions = definitionsPayload["functions"].as<JsonArray>();
                    if (mergedFunctions.isNull())
                    {
                        mergedFunctions = definitionsPayload["functions"].to<JsonArray>();
                    }
                    for (JsonVariant function : doc["payload"]["functions"].as<JsonArray>())
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

                publishEvent("stm32.register", output.c_str());
            }

            else
            {
                Serial.print("Unhandled UART command: ");
                Serial.print(commandName.substring(0, 48));
                Serial.print(" bytes=");
                Serial.println((unsigned int)uartLine.length());
            }
        }
        else
        {
            // Print error to the "debug" serial port
            Serial.print("FROM ESP: deserializeJson() returned ");
            Serial.println(err.c_str());
            char parseMessage[96];
            snprintf(
                parseMessage,
                sizeof(parseMessage),
                "%s bytes=%u",
                err.c_str(),
                (unsigned int)uartLine.length());
            publishEvent("esp32.uart.parse_error", parseMessage, "warning");
        }
    }
}
