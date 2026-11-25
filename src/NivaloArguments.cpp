#include "NivaloArguments.h"

#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace
{
// The number tokens of strict JSON text, in text order. ArduinoJson keeps object members and
// array items in text order, so the k-th number value of a depth-first walk is the k-th token.
class NumberTokens
{
public:
    NumberTokens(const char *text, size_t length) : _end(text + length), _cursor(text) {}

    bool next(const char **start, size_t *length)
    {
        while (_cursor < _end)
        {
            const char c = *_cursor;
            if (c == '"')
            {
                // Skip the string, escapes included.
                for (_cursor++; _cursor < _end && *_cursor != '"'; _cursor++)
                {
                    if (*_cursor == '\\')
                    {
                        _cursor++;
                    }
                }
                _cursor++;
                continue;
            }
            if (c == '-' || (c >= '0' && c <= '9'))
            {
                const char *begin = _cursor;
                while (_cursor < _end && (*_cursor == '-' || *_cursor == '+' || *_cursor == '.' || *_cursor == 'e' ||
                                          *_cursor == 'E' || (*_cursor >= '0' && *_cursor <= '9')))
                {
                    _cursor++;
                }
                *start = begin;
                *length = (size_t)(_cursor - begin);
                return true;
            }
            _cursor++;
        }
        return false;
    }

private:
    const char *_end;
    const char *_cursor;
};

bool integerForm(const char *token, size_t length)
{
    for (size_t i = 0U; i < length; i++)
    {
        if (token[i] == '.' || token[i] == 'e' || token[i] == 'E')
        {
            return false;
        }
    }
    return true;
}

// A nonzero digit before the exponent.
bool nonzeroMantissa(const char *token, size_t length)
{
    for (size_t i = 0U; i < length && token[i] != 'e' && token[i] != 'E'; i++)
    {
        if (token[i] >= '1' && token[i] <= '9')
        {
            return true;
        }
    }
    return false;
}

// Checks every value and gives each non-integer number the correctly rounded binary64 value of
// its original token. ArduinoJson 6.21's own decimal conversion is not correctly rounded: the
// exact decimal of 1 + 2^-52 parses as 1, 1000000000000000.5 as 1000000000000000 and
// 18446744073709551616.0 as ten times its value, so a validator could pass a value the sender
// never meant.
bool representable(JsonVariant value, NumberTokens &tokens)
{
    if (value.isNull())
    {
        // Command arguments contain no null at any depth (section 3).
        return false;
    }
    if (value.is<JsonObject>())
    {
        for (JsonPair member : value.as<JsonObject>())
        {
            if (!representable(member.value(), tokens))
            {
                return false;
            }
        }
        return true;
    }
    if (value.is<JsonArray>())
    {
        for (JsonVariant item : value.as<JsonArray>())
        {
            if (!representable(item, tokens))
            {
                return false;
            }
        }
        return true;
    }
    if (value.is<bool>() || value.is<const char *>())
    {
        return true;
    }
    // A number (is<double>() holds for every number).
    const char *token = NULL;
    size_t length = 0U;
    if (!value.is<double>() || !tokens.next(&token, &length))
    {
        return false;
    }
    if (integerForm(token, length))
    {
        // ArduinoJson keeps an integer token that fits 64 bits as an integer (exact); a larger one
        // would become an approximate float.
        return value.is<int64_t>() || value.is<uint64_t>();
    }
    // The token lies inside the arguments object, so a delimiter follows it and strtod() stops
    // there; it must consume exactly the token.
    char *end = NULL;
    const double number = strtod(token, &end);
    if (end != token + length || !isfinite(number) || (number == 0.0 && nonzeroMantissa(token, length)))
    {
        return false;
    }
    return value.set(number);
}
} // namespace

bool NivaloPreparedArguments::prepare(const char *bytes, size_t length)
{
    release();
    if (bytes == NULL || length == 0U || length > NivaloV11::MaxCommandBytes)
    {
        return false;
    }
    DynamicJsonDocument *document = new (std::nothrow) DynamicJsonDocument(Capacity);
    if (document == NULL)
    {
        return false;
    }
    NumberTokens tokens(bytes, length);
    const char *unused = NULL;
    size_t unusedLength = 0U;
    // Read-only input: ArduinoJson copies the strings into the document.
    const bool parsed = document->capacity() > 0U &&
                        deserializeJson(*document, bytes, length) == DeserializationError::Ok &&
                        document->is<JsonObject>() && representable(document->as<JsonVariant>(), tokens) &&
                        !tokens.next(&unused, &unusedLength);
    if (!parsed)
    {
        delete document;
        return false;
    }
    _document = document;
    return true;
}

void NivaloPreparedArguments::release()
{
    delete _document;
    _document = NULL;
}

JsonObjectConst NivaloPreparedArguments::object() const
{
    return _document == NULL ? JsonObjectConst() : _document->as<JsonObjectConst>();
}
