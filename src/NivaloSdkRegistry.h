#ifndef NIVALO_SDK_REGISTRY_H
#define NIVALO_SDK_REGISTRY_H

#include <Arduino.h>

static constexpr size_t NIVALO_MAX_REGISTERED_FUNCTIONS = 8U;
static constexpr size_t NIVALO_MAX_REGISTERED_VARIABLES = 12U;

// Particle-style function handlers return a non-negative application result on
// success and a negative result on failure. The result is included in the ACK.
typedef int (*NivaloFunctionHandler)(String argument);

struct NivaloFunctionMetadata
{
    const char *displayName = NULL;
    const char *description = NULL;
    const char *argumentExample = NULL;
    const char *returnType = "integer";
    uint16_t timeoutSeconds = 30U;
    const char *dangerLevel = "safe";
    int sortOrder = -1;
};

enum NivaloCommandResult
{
    NIVALO_COMMAND_UNHANDLED = 0,
    NIVALO_COMMAND_SUCCEEDED = 1,
    NIVALO_COMMAND_FAILED = 2
};

// A generic handler may return UNHANDLED to allow the NivaloLink fallback.
typedef NivaloCommandResult (*NivaloCommandHandler)(String command, String arguments, String &message);

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
    String returnType = "integer";
    String dangerLevel = "safe";
    uint16_t timeoutSeconds = 30U;
    int sortOrder = 0;
    NivaloFunctionHandler handler = NULL;
};

struct NivaloRegisteredVariable
{
    String name;
    String unit;
    void *reference = NULL;
    NivaloVariableType type = NIVALO_VARIABLE_INT;
};

class NivaloSdkRegistry
{
public:
    bool addFunction(const char *name, NivaloFunctionHandler handler);
    bool addFunction(const char *name, NivaloFunctionHandler handler, const NivaloFunctionMetadata &metadata);
    bool addVariable(const char *name, void *reference, NivaloVariableType type, const char *unit);
    void setCommandHandler(NivaloCommandHandler handler);

    const NivaloRegisteredFunction *findFunction(const char *name) const;
    NivaloCommandHandler commandHandler() const;
    size_t functionCount() const;
    size_t variableCount() const;
    const NivaloRegisteredFunction &functionAt(size_t index) const;
    const NivaloRegisteredVariable &variableAt(size_t index) const;
    bool formatVariableValue(size_t index, String &value) const;
    const char *variableDataType(size_t index) const;

private:
    bool validName(const char *name) const;
    bool validFunctionMetadata(const NivaloFunctionMetadata &metadata) const;
    NivaloRegisteredFunction _functions[NIVALO_MAX_REGISTERED_FUNCTIONS];
    NivaloRegisteredVariable _variables[NIVALO_MAX_REGISTERED_VARIABLES];
    size_t _functionCount = 0U;
    size_t _variableCount = 0U;
    NivaloCommandHandler _commandHandler = NULL;
};

#endif
