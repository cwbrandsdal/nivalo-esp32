#include "NivaloDevice.h"

#include <ArduinoJson.h>

namespace
{
// Hands each capability packet to a publish function of the device.
class DefinitionsPublisher : public NivaloCatalogSink
{
public:
    typedef bool (*Publish)(void *context, const char *payload, size_t length);
    DefinitionsPublisher(Publish publish, void *context) : _publish(publish), _context(context) {}
    bool emitPacket(const char *payload, size_t length) override { return _publish(_context, payload, length); }

private:
    Publish _publish;
    void *_context;
};
} // namespace

bool NivaloDevice::function(const char *name, NivaloFunctionHandler handler)
{
    return _sdkRegistry.addFunction(name, handler, NivaloFunctionMetadata());
}

bool NivaloDevice::function(const char *name, NivaloFunctionHandler handler, const NivaloFunctionMetadata &metadata)
{
    return _sdkRegistry.addFunction(name, handler, metadata);
}

#define NIVALO_VARIABLE_OVERLOAD(cppType, registryType)                                 \
    bool NivaloDevice::variable(const char *name, cppType *reference, const char *unit)  \
    {                                                                                    \
        return _sdkRegistry.addVariable(name, reference, registryType, unit);            \
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

void NivaloDevice::completeFunction(NivaloFunctionOutcome outcome)
{
    _executor.completeFunction(outcome);
    drainAcks();
}

bool NivaloDevice::sealCatalog()
{
    if (_sdkRegistry.sealed())
    {
        return _catalogReady;
    }
    NivaloCatalogState next;
    const NivaloCatalogDecision decision = nivaloPlanCatalog(_sdkRegistry, _catalogReady ? &_catalog : NULL, &next);
    if (decision == NIVALO_CATALOG_INVALID)
    {
        Serial.println("Nivalo: capability catalog cannot be reported (revision exhausted or too many entries)");
        return false;
    }
    // A new revision is durable before any packet carries it.
    if (decision == NIVALO_CATALOG_CHANGED && !_store.writeCatalog(next))
    {
        Serial.println("Nivalo: capability catalog revision could not be stored; retrying at the next connection");
        return false;
    }
    _catalog = next;
    _catalogReady = true;
    _sdkRegistry.seal();
    return true;
}

void NivaloDevice::publishCatalog()
{
    if (!sealCatalog())
    {
        return;
    }
    DefinitionsPublisher publisher(
        [](void *context, const char *payload, size_t length) -> bool {
            NivaloDevice &device = *static_cast<NivaloDevice *>(context);
            return device.publishEnvelope(device._topics.definitions(), payload, length, NULL, NULL, false);
        },
        this);
    if (!nivaloRefreshCatalog(_sdkRegistry, _catalog, NivaloTelemetryBatch::PayloadCapacity, publisher))
    {
        Serial.println("Nivalo: capability refresh incomplete");
    }
}

bool NivaloDevice::parseArguments(const char *argumentsJson, DynamicJsonDocument &document)
{
    return argumentsJson != NULL && deserializeJson(document, argumentsJson) == DeserializationError::Ok &&
           document.is<JsonObject>();
}

bool NivaloDevice::Platform::hasFunction(const char *name)
{
    return _device._sdkRegistry.findFunction(name) != NULL;
}

bool NivaloDevice::Platform::validArguments(const char *name, const char *argumentsJson)
{
    const NivaloRegisteredFunction *function = _device._sdkRegistry.findFunction(name);
    if (function == NULL)
    {
        return false;
    }
    DynamicJsonDocument arguments(JSON_OBJECT_SIZE(NivaloV11::MaxValues) + NivaloV11::MaxCommandBytes);
    if (!_device.parseArguments(argumentsJson, arguments))
    {
        return false;
    }
    return function->validateArguments == NULL || function->validateArguments(arguments.as<JsonObjectConst>());
}

NivaloFunctionOutcome NivaloDevice::Platform::invokeFunction(const char *name, const char *argumentsJson)
{
    const NivaloRegisteredFunction *function = _device._sdkRegistry.findFunction(name);
    DynamicJsonDocument arguments(JSON_OBJECT_SIZE(NivaloV11::MaxValues) + NivaloV11::MaxCommandBytes);
    if (function == NULL || !_device.parseArguments(argumentsJson, arguments))
    {
        // The claim is durable already; nothing ran, so this is a known zero-effect failure.
        return NIVALO_FUNCTION_FAILED;
    }
    return function->handler(arguments.as<JsonObjectConst>());
}

void NivaloDevice::Platform::refreshCatalog()
{
    _device.publishCatalog();
}
