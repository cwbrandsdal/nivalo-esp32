#include "NivaloSdkRegistry.h"

#include <string.h>

#include "NivaloJsonStrict.h"

namespace
{
bool optionalText(const char *value, size_t maxBytes)
{
    return value == NULL || nivaloIsText(value, maxBytes);
}

bool oneOf(const char *value, const char *const *allowed, size_t count)
{
    for (size_t i = 0U; value != NULL && i < count; i++)
    {
        if (strcmp(value, allowed[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

// argumentExample must parse (strict JSON) as an object (section 4.3, argument-example).
bool validExample(const char *example)
{
    if (example == NULL)
    {
        return true;
    }
    const size_t length = strlen(example);
    NivaloJsonScan scan;
    return length >= 1U && length <= 256U && nivaloIsUtf8(example, length) &&
           nivaloScanJson(example, length, NivaloV11::MaxDepth, NivaloV11::MaxValues, &scan) == NIVALO_JSON_OK &&
           scan.rootIsObject;
}

bool validMetadata(const NivaloFunctionMetadata &metadata)
{
    static const char *const ReturnTypes[] = {"integer", "number", "string", "boolean", "object", "void"};
    static const char *const DangerLevels[] = {"safe", "operational", "disruptive", "dangerous"};
    return optionalText(metadata.displayName, 128U) && optionalText(metadata.description, 256U) &&
           validExample(metadata.argumentExample) &&
           (metadata.returnType == NULL || oneOf(metadata.returnType, ReturnTypes, 6U)) &&
           metadata.timeoutSeconds >= 1U && metadata.timeoutSeconds <= 600U &&
           oneOf(metadata.dangerLevel, DangerLevels, 4U) && metadata.sortOrder >= -1 && metadata.sortOrder <= 32767;
}
} // namespace

bool NivaloSdkRegistry::nameTaken(NivaloCapabilityKind kind, const char *name) const
{
    // Case twins of one kind cannot both be live (section 12.2, name-collision).
    if (kind == NIVALO_KIND_FUNCTION)
    {
        for (size_t i = 0U; i < _functionCount; i++)
        {
            if (nivaloAsciiCaseEqual(_functions[i].name.c_str(), name))
            {
                return true;
            }
        }
        return false;
    }
    for (size_t i = 0U; i < _variableCount; i++)
    {
        if (nivaloAsciiCaseEqual(_variables[i].name.c_str(), name))
        {
            return true;
        }
    }
    return false;
}

bool NivaloSdkRegistry::addFunction(const char *name, NivaloFunctionHandler handler, const NivaloFunctionMetadata &metadata)
{
    if (_sealed || !nivaloIsFunctionName(name) || handler == NULL || _functionCount >= NIVALO_MAX_REGISTERED_FUNCTIONS ||
        nameTaken(NIVALO_KIND_FUNCTION, name) || !validMetadata(metadata))
    {
        return false;
    }
    NivaloRegisteredFunction &registered = _functions[_functionCount];
    registered.name = name;
    registered.displayName = metadata.displayName == NULL ? "" : metadata.displayName;
    registered.description = metadata.description == NULL ? "" : metadata.description;
    registered.argumentExample = metadata.argumentExample == NULL ? "" : metadata.argumentExample;
    registered.returnType = metadata.returnType == NULL ? "" : metadata.returnType;
    registered.timeoutSeconds = metadata.timeoutSeconds;
    registered.dangerLevel = metadata.dangerLevel;
    registered.sortOrder = metadata.sortOrder < 0 ? (int)_functionCount : metadata.sortOrder;
    registered.handler = handler;
    registered.validateArguments = metadata.validateArguments;
    _functionCount++;
    return true;
}

bool NivaloSdkRegistry::addVariable(const char *name, void *reference, NivaloVariableType type, const char *unit)
{
    if (_sealed || !nivaloIsName(name) || reference == NULL || _variableCount >= NIVALO_MAX_REGISTERED_VARIABLES ||
        nameTaken(NIVALO_KIND_VARIABLE, name) || !optionalText(unit, NivaloV11::MaxUnitBytes))
    {
        return false;
    }
    _variables[_variableCount].name = name;
    _variables[_variableCount].unit = unit == NULL ? "" : unit;
    _variables[_variableCount].reference = reference;
    _variables[_variableCount].type = type;
    _variableCount++;
    return true;
}

const NivaloRegisteredFunction *NivaloSdkRegistry::findFunction(const char *name) const
{
    for (size_t i = 0U; name != NULL && i < _functionCount; i++)
    {
        if (_functions[i].name == name)
        {
            return &_functions[i];
        }
    }
    return NULL;
}

bool NivaloSdkRegistry::readVariable(size_t index, double *number, String *raw) const
{
    if (index >= _variableCount || _variables[index].reference == NULL || number == NULL || raw == NULL)
    {
        return false;
    }
    const NivaloRegisteredVariable &variable = _variables[index];
    *raw = "";
    switch (variable.type)
    {
    case NIVALO_VARIABLE_INT: *number = *(int *)variable.reference; return true;
    case NIVALO_VARIABLE_UINT: *number = *(unsigned int *)variable.reference; return true;
    case NIVALO_VARIABLE_LONG: *number = (double)*(long *)variable.reference; return true;
    case NIVALO_VARIABLE_ULONG: *number = (double)*(unsigned long *)variable.reference; return true;
    case NIVALO_VARIABLE_FLOAT: *number = *(float *)variable.reference; return true;
    case NIVALO_VARIABLE_DOUBLE: *number = *(double *)variable.reference; return true;
    case NIVALO_VARIABLE_BOOL: *raw = *(bool *)variable.reference ? "true" : "false"; return true;
    case NIVALO_VARIABLE_STRING: *raw = *(String *)variable.reference; return true;
    }
    return false;
}

const char *NivaloSdkRegistry::variableDataType(size_t index) const
{
    if (index >= _variableCount)
    {
        return "string";
    }
    switch (_variables[index].type)
    {
    case NIVALO_VARIABLE_BOOL: return "boolean";
    case NIVALO_VARIABLE_STRING: return "string";
    default: return "number";
    }
}

NivaloCapabilityKind NivaloSdkRegistry::capabilityKind(size_t index) const
{
    return index < _variableCount ? NIVALO_KIND_VARIABLE : NIVALO_KIND_FUNCTION;
}

const char *NivaloSdkRegistry::capabilityName(size_t index) const
{
    return index < _variableCount ? _variables[index].name.c_str() : _functions[index - _variableCount].name.c_str();
}

void NivaloSdkRegistry::writeDefinition(size_t index, NivaloJsonWriter &writer) const
{
    writer.beginObject();
    if (index < _variableCount)
    {
        const NivaloRegisteredVariable &variable = _variables[index];
        writer.key("dataType");
        writer.string(variableDataType(index));
        if (variable.unit.length() > 0U)
        {
            writer.key("unit");
            writer.string(variable.unit.c_str());
        }
        writer.key("historyEnabled");
        writer.boolean(true);
        writer.key("sortOrder");
        writer.uint(index);
        writer.endObject();
        return;
    }
    const NivaloRegisteredFunction &function = _functions[index - _variableCount];
    if (function.displayName.length() > 0U)
    {
        writer.key("displayName");
        writer.string(function.displayName.c_str());
    }
    if (function.description.length() > 0U)
    {
        writer.key("description");
        writer.string(function.description.c_str());
    }
    if (function.argumentExample.length() > 0U)
    {
        writer.key("argumentExample");
        writer.string(function.argumentExample.c_str());
    }
    if (function.returnType.length() > 0U)
    {
        writer.key("returnType");
        writer.string(function.returnType.c_str());
    }
    writer.key("timeoutSeconds");
    writer.uint(function.timeoutSeconds);
    writer.key("dangerLevel");
    writer.string(function.dangerLevel.c_str());
    writer.key("sortOrder");
    writer.uint((uint32_t)function.sortOrder);
    writer.endObject();
}
