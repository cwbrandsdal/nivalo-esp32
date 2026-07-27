#ifndef NIVALO_COMMAND_RESULT_CACHE_H
#define NIVALO_COMMAND_RESULT_CACHE_H

#include <stddef.h>
#include <string.h>

static constexpr size_t NIVALO_COMPLETED_COMMAND_CACHE_CAPACITY = 8U;
static constexpr size_t NIVALO_COMMAND_ID_CAPACITY = 40U;
static constexpr size_t NIVALO_COMMAND_NAME_CAPACITY = 65U;
static constexpr size_t NIVALO_COMMAND_STATUS_CAPACITY = 16U;
static constexpr size_t NIVALO_COMMAND_MESSAGE_CAPACITY = 96U;

struct NivaloCompletedCommand
{
    char commandId[NIVALO_COMMAND_ID_CAPACITY] = {0};
    char commandName[NIVALO_COMMAND_NAME_CAPACITY] = {0};
    char status[NIVALO_COMMAND_STATUS_CAPACITY] = {0};
    char message[NIVALO_COMMAND_MESSAGE_CAPACITY] = {0};
};

class NivaloCommandResultCache
{
public:
    const NivaloCompletedCommand *find(const char *commandId) const
    {
        if (!valid(commandId, NIVALO_COMMAND_ID_CAPACITY))
        {
            return NULL;
        }
        for (size_t index = 0U; index < NIVALO_COMPLETED_COMMAND_CACHE_CAPACITY; index++)
        {
            if (_occupied[index] && strcmp(_entries[index].commandId, commandId) == 0)
            {
                return &_entries[index];
            }
        }
        return NULL;
    }

    bool remember(
        const char *commandId,
        const char *commandName,
        const char *status,
        const char *message)
    {
        if (!valid(commandId, NIVALO_COMMAND_ID_CAPACITY) ||
            !valid(commandName, NIVALO_COMMAND_NAME_CAPACITY) ||
            !valid(status, NIVALO_COMMAND_STATUS_CAPACITY) ||
            message == NULL)
        {
            return false;
        }

        if (find(commandId) != NULL)
        {
            return true;
        }

        copy(_entries[_next].commandId, NIVALO_COMMAND_ID_CAPACITY, commandId);
        copy(_entries[_next].commandName, NIVALO_COMMAND_NAME_CAPACITY, commandName);
        copy(_entries[_next].status, NIVALO_COMMAND_STATUS_CAPACITY, status);
        copy(_entries[_next].message, NIVALO_COMMAND_MESSAGE_CAPACITY, message);
        _occupied[_next] = true;
        _next = (_next + 1U) % NIVALO_COMPLETED_COMMAND_CACHE_CAPACITY;
        return true;
    }

private:
    static bool valid(const char *value, size_t capacity)
    {
        return value != NULL && value[0] != '\0' && strlen(value) < capacity;
    }

    static void copy(char *destination, size_t capacity, const char *source)
    {
        strncpy(destination, source, capacity - 1U);
        destination[capacity - 1U] = '\0';
    }

    NivaloCompletedCommand _entries[NIVALO_COMPLETED_COMMAND_CACHE_CAPACITY];
    bool _occupied[NIVALO_COMPLETED_COMMAND_CACHE_CAPACITY] = {false};
    size_t _next = 0U;
};

#endif
