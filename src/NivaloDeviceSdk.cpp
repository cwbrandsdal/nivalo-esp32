#include "NivaloDevice.h"

#include <ArduinoJson.h>

namespace
{
// Retry interval for a capability refresh that did not complete.
static constexpr int64_t CatalogRetryUs = 30LL * 1000000LL;

// Guards the asynchronous completion mailbox: completeFunction() may run on any task.
portMUX_TYPE completionLock = portMUX_INITIALIZER_UNLOCKED;

// Hands each capability packet to a publish function of the device and counts the packets.
class DefinitionsPublisher : public NivaloCatalogSink
{
public:
    typedef bool (*Publish)(void *context, const char *payload, size_t length);
    DefinitionsPublisher(Publish publish, void *context) : _publish(publish), _context(context) {}
    bool emitPacket(const char *payload, size_t length) override
    {
        const bool published = _publish(_context, payload, length);
        if (published)
        {
            _published++;
        }
        return published;
    }
    unsigned published() const { return _published; }

private:
    Publish _publish;
    void *_context;
    unsigned _published = 0U;
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

// --- Asynchronous completion ---------------------------------------------------------------------

uint32_t NivaloDevice::functionToken() const
{
    portENTER_CRITICAL(&completionLock);
    const uint32_t token = _completionToken;
    portEXIT_CRITICAL(&completionLock);
    return token;
}

bool NivaloDevice::completeFunction(uint32_t token, NivaloFunctionOutcome outcome)
{
    // Only queued here, whatever task calls it; the journal write happens on the executor.
    if (token == 0U || outcome == NIVALO_FUNCTION_PENDING)
    {
        return false;
    }
    bool queued = false;
    portENTER_CRITICAL(&completionLock);
    if (token == _completionToken && !_completionQueued)
    {
        _completionOutcome = outcome;
        _completionQueued = true;
        queued = true;
    }
    portEXIT_CRITICAL(&completionLock);
    return queued;
}

void NivaloDevice::serviceCompletion()
{
    uint32_t token = 0U;
    NivaloFunctionOutcome outcome = NIVALO_FUNCTION_UNKNOWN;
    portENTER_CRITICAL(&completionLock);
    if (_completionQueued)
    {
        token = _completionToken;
        outcome = _completionOutcome;
        _completionQueued = false;
    }
    portEXIT_CRITICAL(&completionLock);
    if (token == 0U)
    {
        return;
    }
    // The executor matches the token and ends busy only once the result is durable. A failed
    // write blocks the executor; either way this invocation takes no further completion.
    (void)_executor.completeFunction(token, outcome);
    portENTER_CRITICAL(&completionLock);
    if (_completionToken == token)
    {
        _completionToken = 0U;
    }
    portEXIT_CRITICAL(&completionLock);
    drainAcks();
}

// --- Capability catalog --------------------------------------------------------------------------

bool NivaloDevice::sealCatalog()
{
    if (_sdkRegistry.sealed())
    {
        return _catalogReady;
    }
    if (_catalogRead == NIVALO_STORE_CORRUPT)
    {
        // Unreadable is not a first enrollment: read again, never write over it.
        NivaloCatalogState stored;
        _catalogRead = _store.loadCatalog(&stored);
        if (_catalogRead == NIVALO_STORE_OK)
        {
            _catalog = stored;
        }
        else if (_catalogRead == NIVALO_STORE_CORRUPT)
        {
            Serial.println("Nivalo: stored capability catalog unreadable; refresh deferred");
            return false;
        }
    }
    NivaloCatalogState next;
    const NivaloCatalogDecision decision =
        nivaloPlanCatalog(_sdkRegistry, _catalogRead == NIVALO_STORE_OK ? &_catalog : NULL, &next);
    if (decision == NIVALO_CATALOG_INVALID)
    {
        Serial.println("Nivalo: capability catalog cannot be reported (revision exhausted or too many entries)");
        _catalogUnreportable = true;
        return false;
    }
    if (decision == NIVALO_CATALOG_REFUSED)
    {
        Serial.println("Nivalo: capability catalog change refused: more than 16 retired names, or a name with two "
                       "retired case twins; the reported catalog stays as it is");
        _catalogUnreportable = true;
        return false;
    }
    // A new revision is durable before any packet carries it.
    if (decision == NIVALO_CATALOG_CHANGED && !_store.writeCatalog(next))
    {
        Serial.println("Nivalo: capability catalog revision could not be stored; retrying");
        return false;
    }
    _catalog = next;
    _catalogRead = NIVALO_STORE_OK;
    _catalogReady = true;
    _sdkRegistry.seal();
    return true;
}

NivaloRefreshOutcome NivaloDevice::publishCatalog()
{
    _catalogRefreshPending = true;
    _catalogRetryAtUs = monotonicUs() + CatalogRetryUs;
    if (!sealCatalog())
    {
        // A refused or invalid catalog stays so until the firmware changes: no retries.
        _catalogRefreshPending = !_catalogUnreportable;
        return NIVALO_REFRESH_NOT_STARTED;
    }
    DefinitionsPublisher publisher(
        [](void *context, const char *payload, size_t length) -> bool {
            NivaloDevice &device = *static_cast<NivaloDevice *>(context);
            return device.publishEnvelope(device._topics.definitions(), payload, length, NULL, NULL, false);
        },
        this);
    if (nivaloRefreshCatalog(_sdkRegistry, _catalog, NivaloTelemetryBatch::PayloadCapacity, publisher))
    {
        _catalogRefreshPending = false;
        return NIVALO_REFRESH_COMPLETE;
    }
    Serial.println("Nivalo: capability refresh incomplete; retrying");
    return publisher.published() == 0U ? NIVALO_REFRESH_NOT_STARTED : NIVALO_REFRESH_PARTIAL;
}

void NivaloDevice::serviceCatalogRetry()
{
    if (_catalogRefreshPending && _connection.connected() && clockValid() && monotonicUs() >= _catalogRetryAtUs)
    {
        (void)publishCatalog();
    }
}

// --- Generic commands ----------------------------------------------------------------------------

bool NivaloDevice::Platform::hasFunction(const char *name)
{
    return _device._sdkRegistry.findFunction(name) != NULL;
}

bool NivaloDevice::Platform::prepareFunction(const NivaloOperation &operation)
{
    // Parsed once from the bytes received; the validator and later the handler see this object.
    _device._preparedFunction = _device._sdkRegistry.findFunction(operation.command);
    if (_device._preparedFunction == NULL || !_device._prepared.prepare(operation.arguments, operation.argumentsLength))
    {
        releasePrepared();
        return false;
    }
    const NivaloArgumentValidator validator = _device._preparedFunction->validateArguments;
    if (validator != NULL && !validator(_device._prepared.object()))
    {
        releasePrepared();
        return false;
    }
    return true;
}

NivaloFunctionOutcome NivaloDevice::Platform::invokePrepared(uint32_t token)
{
    const NivaloRegisteredFunction *function = _device._preparedFunction;
    if (function == NULL || !_device._prepared.ready())
    {
        // The claim is durable already; nothing ran, so this is a known zero-effect failure.
        return NIVALO_FUNCTION_FAILED;
    }
    // Completions for this invocation are accepted from now on, also from a task the handler
    // starts before it returns.
    portENTER_CRITICAL(&completionLock);
    _device._completionToken = token;
    _device._completionQueued = false;
    portEXIT_CRITICAL(&completionLock);
    const NivaloFunctionOutcome outcome = function->handler(_device._prepared.object());
    if (outcome != NIVALO_FUNCTION_PENDING)
    {
        // A synchronous result is final; anything queued for this token is dropped.
        portENTER_CRITICAL(&completionLock);
        _device._completionToken = 0U;
        _device._completionQueued = false;
        portEXIT_CRITICAL(&completionLock);
    }
    return outcome;
}

void NivaloDevice::Platform::releasePrepared()
{
    _device._prepared.release();
    _device._preparedFunction = NULL;
}

NivaloRefreshOutcome NivaloDevice::Platform::refreshCatalog()
{
    return _device.publishCatalog();
}
