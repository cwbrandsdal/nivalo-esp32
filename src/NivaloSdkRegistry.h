#ifndef NIVALO_SDK_REGISTRY_H
#define NIVALO_SDK_REGISTRY_H

#include <Arduino.h>
#include <ArduinoJson.h>

#include "NivaloCatalog.h"
#include "NivaloExecutor.h"

static constexpr size_t NIVALO_MAX_REGISTERED_FUNCTIONS = 8U;
static constexpr size_t NIVALO_MAX_REGISTERED_VARIABLES = 12U;

// A function handler receives the command's arguments object (never containing null) and
// reports what happened:
//   NIVALO_FUNCTION_SUCCEEDED  the effect completed
//   NIVALO_FUNCTION_FAILED     the effect certainly did not happen
//   NIVALO_FUNCTION_UNKNOWN    the effect may have happened (for example a timeout after an
//                              output changed); reported as indeterminate, never retried
//   NIVALO_FUNCTION_PENDING    still running; call NivaloDevice::completeFunction() later
typedef NivaloFunctionOutcome (*NivaloFunctionHandler)(JsonObjectConst arguments);
// Checks the arguments before the operation is claimed. Return false to reject the command as
// invalid_command without calling the handler.
typedef bool (*NivaloArgumentValidator)(JsonObjectConst arguments);

struct NivaloFunctionMetadata
{
    const char *displayName = NULL;  // text, at most 128 bytes
    const char *description = NULL;  // text, at most 256 bytes
    // Example arguments as a JSON object text, at most 256 bytes. Display only.
    const char *argumentExample = NULL;
    // integer, number, string, boolean, object or void; NULL omits it.
    const char *returnType = NULL;
    uint16_t timeoutSeconds = 30U; // 1-600
    const char *dangerLevel = "safe"; // safe, operational, disruptive or dangerous
    int sortOrder = -1;               // 0-32767; -1 uses the registration order
    // NULL accepts any arguments object.
    NivaloArgumentValidator validateArguments = NULL;
};

enum NivaloVariableType
{
    NIVALO_VARIABLE_INT,
    NIVALO_VARIABLE_UINT,
    NIVALO_VARIABLE_LONG,
    NIVALO_VARIABLE_ULONG,
    NIVALO_VARIABLE_FLOAT,
    NIVALO_VARIABLE_DOUBLE,
    NIVALO_VARIABLE_BOOL,
    NIVALO_VARIABLE_STRING
};

struct NivaloRegisteredFunction
{
    String name;
    String displayName;
    String description;
    String argumentExample;
    String returnType;
    String dangerLevel = "safe";
    uint16_t timeoutSeconds = 30U;
    int sortOrder = 0;
    NivaloFunctionHandler handler = NULL;
    NivaloArgumentValidator validateArguments = NULL;
};

struct NivaloRegisteredVariable
{
    String name;
    String unit;
    void *reference = NULL;
    NivaloVariableType type = NIVALO_VARIABLE_INT;
};

// Registered functions and variables. As a catalog source, variables come first, then functions,
// each in registration order.
class NivaloSdkRegistry : public NivaloCatalogSource
{
public:
    bool addFunction(const char *name, NivaloFunctionHandler handler, const NivaloFunctionMetadata &metadata);
    bool addVariable(const char *name, void *reference, NivaloVariableType type, const char *unit);
    // After the catalog is published, registrations are refused so the reported catalog and its
    // revision stay consistent for the whole run.
    void seal() { _sealed = true; }
    bool sealed() const { return _sealed; }

    const NivaloRegisteredFunction *findFunction(const char *name) const;
    size_t functionCount() const { return _functionCount; }
    size_t variableCount() const { return _variableCount; }
    const NivaloRegisteredFunction &functionAt(size_t index) const { return _functions[index]; }
    const NivaloRegisteredVariable &variableAt(size_t index) const { return _variables[index]; }
    // The variable's current value: a number (formatted as an exact decimal), a boolean as raw
    // "true"/"false", or a string as raw text.
    bool readVariable(size_t index, double *number, String *raw) const;
    const char *variableDataType(size_t index) const;

    size_t capabilityCount() const override { return _variableCount + _functionCount; }
    NivaloCapabilityKind capabilityKind(size_t index) const override;
    const char *capabilityName(size_t index) const override;
    void writeDefinition(size_t index, NivaloJsonWriter &writer) const override;

private:
    bool nameTaken(NivaloCapabilityKind kind, const char *name) const;
    NivaloRegisteredFunction _functions[NIVALO_MAX_REGISTERED_FUNCTIONS];
    NivaloRegisteredVariable _variables[NIVALO_MAX_REGISTERED_VARIABLES];
    size_t _functionCount = 0U;
    size_t _variableCount = 0U;
    bool _sealed = false;
};

#endif
