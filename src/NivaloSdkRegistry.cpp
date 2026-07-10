#include "NivaloSdkRegistry.h"

#include <string.h>

bool NivaloSdkRegistry::validName(const char *name) const
{
    return name != NULL && name[0] != '\0' && strlen(name) <= 64U;
}

bool NivaloSdkRegistry::addFunction(const char *name, NivaloFunctionHandler handler)
{
    if (!validName(name) || handler == NULL || _functionCount >= NIVALO_MAX_REGISTERED_FUNCTIONS ||
        findFunction(name) != NULL)
    {
        return false;
    }
    _functions[_functionCount].name = name;
    _functions[_functionCount].handler = handler;
    _functionCount++;
    return true;
}

bool NivaloSdkRegistry::addVariable(const char *name, void *reference, NivaloVariableType type, const char *unit)
{
    if (!validName(name) || reference == NULL || _variableCount >= NIVALO_MAX_REGISTERED_VARIABLES)
    {
        return false;
    }
    for (size_t i = 0; i < _variableCount; i++)
    {
        if (_variables[i].name == name)
        {
            return false;
        }
    }
    _variables[_variableCount].name = name;
    _variables[_variableCount].unit = unit == NULL ? "" : unit;
    _variables[_variableCount].reference = reference;
    _variables[_variableCount].type = type;
    _variableCount++;
    return true;
}

void NivaloSdkRegistry::setCommandHandler(NivaloCommandHandler handler)
{
    _commandHandler = handler;
}

const NivaloRegisteredFunction *NivaloSdkRegistry::findFunction(const char *name) const
{
    if (name == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < _functionCount; i++)
    {
        if (_functions[i].name == name)
        {
            return &_functions[i];
        }
    }
    return NULL;
}

NivaloCommandHandler NivaloSdkRegistry::commandHandler() const
{
    return _commandHandler;
}

size_t NivaloSdkRegistry::functionCount() const
{
    return _functionCount;
}

size_t NivaloSdkRegistry::variableCount() const
{
    return _variableCount;
}

const NivaloRegisteredFunction &NivaloSdkRegistry::functionAt(size_t index) const
{
    return _functions[index];
}

const NivaloRegisteredVariable &NivaloSdkRegistry::variableAt(size_t index) const
{
    return _variables[index];
}

bool NivaloSdkRegistry::formatVariableValue(size_t index, String &value) const
{
    if (index >= _variableCount || _variables[index].reference == NULL)
    {
        return false;
    }
    const NivaloRegisteredVariable &variable = _variables[index];
    switch (variable.type)
    {
    case NIVALO_VARIABLE_INT:
        value = String(*(int *)variable.reference);
        break;
    case NIVALO_VARIABLE_UINT:
        value = String(*(unsigned int *)variable.reference);
        break;
    case NIVALO_VARIABLE_LONG:
        value = String(*(long *)variable.reference);
        break;
    case NIVALO_VARIABLE_ULONG:
        value = String(*(unsigned long *)variable.reference);
        break;
    case NIVALO_VARIABLE_FLOAT:
        value = String(*(float *)variable.reference, 6);
        break;
    case NIVALO_VARIABLE_DOUBLE:
        value = String(*(double *)variable.reference, 6);
        break;
    case NIVALO_VARIABLE_BOOL:
        value = *(bool *)variable.reference ? "true" : "false";
        break;
    case NIVALO_VARIABLE_STRING:
        value = *(String *)variable.reference;
        break;
    default:
        return false;
    }
    return true;
}

const char *NivaloSdkRegistry::variableDataType(size_t index) const
{
    if (index >= _variableCount)
    {
        return "string";
    }
    switch (_variables[index].type)
    {
    case NIVALO_VARIABLE_BOOL:
        return "boolean";
    case NIVALO_VARIABLE_STRING:
        return "string";
    default:
        return "number";
    }
}
