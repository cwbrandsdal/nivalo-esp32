#ifndef NIVALO_ARGUMENTS_H
#define NIVALO_ARGUMENTS_H

// The arguments of one generic command, parsed once from the exact bytes received
// (NivaloOperation::arguments) into the object the validator and then the handler see. Nothing is
// re-serialized in between, so a value cannot change between validation and the effect.
// Arduino-free; the host tests drive the same code.

#include <ArduinoJson.h>
#include <stddef.h>

#include "NivaloWire.h"

class NivaloPreparedArguments
{
public:
    NivaloPreparedArguments() {}
    ~NivaloPreparedArguments() { release(); }
    NivaloPreparedArguments(const NivaloPreparedArguments &) = delete;
    NivaloPreparedArguments &operator=(const NivaloPreparedArguments &) = delete;

    // bytes is a JSON object that already passed the strict scanner. False (and nothing held)
    // when it cannot be parsed, is not an object, contains null, or has a number the parsed value
    // cannot represent: a non-finite value, an integer outside 64 bits, or a nonzero number that
    // underflows to zero.
    bool prepare(const char *bytes, size_t length);
    void release();
    bool ready() const { return _document != NULL; }
    // Valid while ready().
    JsonObjectConst object() const;

    // Capacity of the parsed document: every value of a command at its largest.
    static const size_t Capacity = JSON_OBJECT_SIZE(NivaloV11::MaxValues) + NivaloV11::MaxCommandBytes;

private:
    DynamicJsonDocument *_document = NULL;
};

#endif
