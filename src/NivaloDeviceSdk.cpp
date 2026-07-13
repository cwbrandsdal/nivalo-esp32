#include "NivaloDevice.h"

#include <ArduinoJson.h>

namespace
{
static constexpr size_t DefinitionsJsonCapacity = 3072U;

void addFirmwareTargets(JsonVariant firmware)
{
    JsonArray targets = firmware["targets"].to<JsonArray>();
    targets.add("esp32");
#if NIVALO_HAS_SECONDARY_MCU
    targets.add("stm32");
#endif
}
} // namespace

bool NivaloDevice::function(const char *name, NivaloFunctionHandler handler)
{
    bool registered = _sdkRegistry.addFunction(name, handler);
    if (registered && _connection.connected() && _deviceId.length() > 0U)
    {
        publishDefinitions();
    }
    return registered;
}

#define NIVALO_VARIABLE_OVERLOAD(cppType, registryType)                                      \
    bool NivaloDevice::variable(const char *name, cppType *reference, const char *unit)       \
    {                                                                                         \
        bool registered = _sdkRegistry.addVariable(name, reference, registryType, unit);      \
        if (registered && _connection.connected() && _deviceId.length() > 0U)                  \
        {                                                                                     \
            publishDefinitions();                                                             \
        }                                                                                     \
        return registered;                                                                    \
    }

NIVALO_VARIABLE_OVERLOAD(int, NIVALO_VARIABLE_INT)
NIVALO_VARIABLE_OVERLOAD(unsigned int, NIVALO_VARIABLE_UINT)
NIVALO_VARIABLE_OVERLOAD(long, NIVALO_VARIABLE_LONG)
NIVALO_VARIABLE_OVERLOAD(unsigned long, NIVALO_VARIABLE_ULONG)
NIVALO_VARIABLE_OVERLOAD(float, NIVALO_VARIABLE_FLOAT)
NIVALO_VARIABLE_OVERLOAD(double, NIVALO_VARIABLE_DOUBLE)
NIVALO_VARIABLE_OVERLOAD(bool, NIVALO_VARIABLE_BOOL)
NIVALO_VARIABLE_OVERLOAD(String, NIVALO_VARIABLE_STRING)

#undef NIVALO_VARIABLE_OVERLOAD

void NivaloDevice::onCommand(NivaloCommandHandler handler)
{
    _sdkRegistry.setCommandHandler(handler);
}

void NivaloDevice::appendSdkDefinitions(JsonObject payload)
{
    if (_sdkRegistry.variableCount() > 0U)
    {
        JsonArray variables = payload["variables"].to<JsonArray>();
        for (size_t i = 0; i < _sdkRegistry.variableCount(); i++)
        {
            const NivaloRegisteredVariable &registered = _sdkRegistry.variableAt(i);
            JsonObject definition = variables.createNestedObject();
            definition["name"] = registered.name;
            definition["dataType"] = _sdkRegistry.variableDataType(i);
            if (registered.unit.length() > 0U)
            {
                definition["unit"] = registered.unit;
            }
            definition["historyEnabled"] = true;
            definition["sortOrder"] = (int)i;
        }
    }

    if (_sdkRegistry.functionCount() > 0U)
    {
        JsonArray functions = payload["functions"].to<JsonArray>();
        for (size_t i = 0; i < _sdkRegistry.functionCount(); i++)
        {
            const NivaloRegisteredFunction &registered = _sdkRegistry.functionAt(i);
            JsonObject definition = functions.createNestedObject();
            definition["name"] = registered.name;
            definition["returnType"] = "integer";
            definition["timeoutSeconds"] = 30;
            definition["dangerLevel"] = "safe";
            definition["sortOrder"] = (int)i;
        }
    }
}

bool NivaloDevice::publishDefinitions()
{
    if (!_connection.connected() || _deviceId.length() == 0U ||
        (_sdkRegistry.functionCount() == 0U && _sdkRegistry.variableCount() == 0U))
    {
        return false;
    }

    DynamicJsonDocument definitions(DefinitionsJsonCapacity);
    definitions["schema"] = "nivalo.iot.v1";
    definitions["messageId"] = createMessageId();
    definitions["deviceId"] = _deviceId;
    definitions["sentAt"] = createTimestamp();
    definitions["firmware"]["version"] = _firmwareVersion;
    definitions["firmware"]["hardware"] = _hardwareName;
    addFirmwareTargets(definitions["firmware"]);
    appendSdkDefinitions(definitions["payload"].to<JsonObject>());

    String output;
    serializeJson(definitions, output);
    if (definitions.overflowed())
    {
        queueEventReport("esp32.definitions.publish.failed", "local definitions exceed JSON capacity", "warning");
        return false;
    }
    bool published = _connection.publish(_definitionsTopic, output.c_str());
    if (!published)
    {
        queueEventReport("esp32.definitions.publish.failed", "local definitions MQTT publish failed", "warning");
    }
    return published;
}

size_t NivaloDevice::publishVariables()
{
    if (!_connection.connected())
    {
        return 0U;
    }
    size_t published = 0U;
    for (size_t i = 0; i < _sdkRegistry.variableCount(); i++)
    {
        String value;
        if (_sdkRegistry.formatVariableValue(i, value))
        {
            const NivaloRegisteredVariable &registered = _sdkRegistry.variableAt(i);
            published += publishTelemetry(
                registered.name.c_str(),
                value.c_str(),
                registered.unit.length() > 0U ? registered.unit.c_str() : NULL);
        }
    }
    return published;
}

bool NivaloDevice::dispatchSdkCommand(const char *commandId, const String &commandName, const String &arguments)
{
    if (commandName == "requestDefinitions")
    {
        bool published = publishDefinitions();
#if NIVALO_HAS_SECONDARY_MCU
        // Publish local registrations now, then let the unmatched fallback ask
        // the secondary MCU for its definitions as well.
        (void)published;
        return false;
#else
        if (commandId != NULL && commandId[0] != '\0')
        {
            publishCommandAck(
                commandId,
                published ? "succeeded" : "failed",
                published ? "ESP32 definitions published" : "ESP32 definitions publish failed");
        }
        return true;
#endif
    }

    const NivaloRegisteredFunction *registeredFunction = _sdkRegistry.findFunction(commandName.c_str());
    if (registeredFunction != NULL)
    {
        int result = registeredFunction->handler(arguments);
        char ackMessage[96];
        snprintf(ackMessage, sizeof(ackMessage), "Function %s returned %d", commandName.c_str(), result);
        if (commandId != NULL && commandId[0] != '\0')
        {
            publishCommandAck(commandId, result >= 0 ? "succeeded" : "failed", ackMessage);
        }
        publishEvent(
            result >= 0 ? "esp32.function.succeeded" : "esp32.function.failed",
            commandName.c_str(),
            result >= 0 ? "info" : "warning");
        return true;
    }

    NivaloCommandHandler handler = _sdkRegistry.commandHandler();
    if (handler == NULL)
    {
        return false;
    }
    String message;
    NivaloCommandResult result = handler(commandName, arguments, message);
    if (result == NIVALO_COMMAND_UNHANDLED)
    {
        return false;
    }
    const bool succeeded = result == NIVALO_COMMAND_SUCCEEDED;
    if (message.length() == 0U)
    {
        message = succeeded ? "Command handler completed" : "Command handler failed";
    }
    if (commandId != NULL && commandId[0] != '\0')
    {
        publishCommandAck(commandId, succeeded ? "succeeded" : "failed", message.c_str());
    }
    publishEvent(
        succeeded ? "esp32.command-handler.succeeded" : "esp32.command-handler.failed",
        commandName.c_str(),
        succeeded ? "info" : "warning");
    return true;
}

