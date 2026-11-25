#include "NivaloJsonStrict.h"

#include <string.h>

#include "NivaloWire.h"

namespace
{
enum Container : uint8_t
{
    CONTAINER_OBJECT,
    CONTAINER_ARRAY
};

struct Span
{
    uint16_t offset;
    uint16_t length;
};

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex4(const char *data, size_t length, size_t position, uint32_t *unit)
{
    if (length - position < 4U)
    {
        return false;
    }
    uint32_t value = 0U;
    for (size_t i = 0U; i < 4U; i++)
    {
        const int digit = hexValue(data[position + i]);
        if (digit < 0)
        {
            return false;
        }
        value = (value << 4U) | (uint32_t)digit;
    }
    *unit = value;
    return true;
}

class Scanner
{
public:
    Scanner(const char *data, size_t length) : _data(data), _length(length) {}

    void skipWhitespace()
    {
        while (_position < _length)
        {
            const char c = _data[_position];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            {
                return;
            }
            _position++;
        }
    }

    bool atEnd() const { return _position >= _length; }
    char peek() const { return _position < _length ? _data[_position] : '\0'; }
    size_t position() const { return _position; }
    void advance() { _position++; }

    // Scans a string starting at the opening quote. Returns OK, SYNTAX or ENCODING.
    NivaloJsonRule string(Span *span)
    {
        if (peek() != '"')
        {
            return NIVALO_JSON_SYNTAX;
        }
        _position++;
        const size_t start = _position;
        while (_position < _length)
        {
            const unsigned char c = (unsigned char)_data[_position];
            if (c == '"')
            {
                if (span != NULL)
                {
                    span->offset = (uint16_t)start;
                    span->length = (uint16_t)(_position - start);
                }
                _position++;
                return NIVALO_JSON_OK;
            }
            if (c < 0x20U)
            {
                return NIVALO_JSON_SYNTAX;
            }
            if (c != '\\')
            {
                _position++;
                continue;
            }
            if (_length - _position < 2U)
            {
                return NIVALO_JSON_SYNTAX;
            }
            const char escape = _data[_position + 1U];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
                escape == 'n' || escape == 'r' || escape == 't')
            {
                _position += 2U;
                continue;
            }
            if (escape != 'u')
            {
                return NIVALO_JSON_SYNTAX;
            }
            uint32_t unit;
            if (!hex4(_data, _length, _position + 2U, &unit))
            {
                return NIVALO_JSON_SYNTAX;
            }
            _position += 6U;
            if (unit == 0U)
            {
                return NIVALO_JSON_ENCODING;
            }
            if (unit >= 0xDC00U && unit <= 0xDFFFU)
            {
                return NIVALO_JSON_ENCODING;
            }
            if (unit >= 0xD800U && unit <= 0xDBFFU)
            {
                uint32_t low;
                if (_length - _position < 6U || _data[_position] != '\\' || _data[_position + 1U] != 'u' ||
                    !hex4(_data, _length, _position + 2U, &low) || low < 0xDC00U || low > 0xDFFFU)
                {
                    return NIVALO_JSON_ENCODING;
                }
                _position += 6U;
            }
        }
        return NIVALO_JSON_SYNTAX;
    }

    bool number()
    {
        if (peek() == '-')
        {
            _position++;
        }
        if (peek() == '0')
        {
            _position++;
        }
        else if (peek() >= '1' && peek() <= '9')
        {
            while (peek() >= '0' && peek() <= '9') _position++;
        }
        else
        {
            return false;
        }
        if (peek() == '.')
        {
            _position++;
            if (!(peek() >= '0' && peek() <= '9')) return false;
            while (peek() >= '0' && peek() <= '9') _position++;
        }
        if (peek() == 'e' || peek() == 'E')
        {
            _position++;
            if (peek() == '+' || peek() == '-') _position++;
            if (!(peek() >= '0' && peek() <= '9')) return false;
            while (peek() >= '0' && peek() <= '9') _position++;
        }
        return true;
    }

    bool literal(const char *text)
    {
        const size_t length = strlen(text);
        if (_length - _position < length || memcmp(_data + _position, text, length) != 0)
        {
            return false;
        }
        _position += length;
        return true;
    }

private:
    const char *_data;
    size_t _length;
    size_t _position = 0U;
};

// Decodes one character of validated JSON string content.
uint32_t nextCharacter(const char *data, size_t end, size_t *position)
{
    if (data[*position] != '\\')
    {
        uint32_t codePoint = 0U;
        (void)nivaloUtf8Next(data, end, position, &codePoint);
        return codePoint;
    }
    const char escape = data[*position + 1U];
    *position += 2U;
    switch (escape)
    {
    case 'b': return 0x08U;
    case 'f': return 0x0CU;
    case 'n': return 0x0AU;
    case 'r': return 0x0DU;
    case 't': return 0x09U;
    case 'u':
    {
        uint32_t unit = 0U;
        (void)hex4(data, end, *position, &unit);
        *position += 4U;
        if (unit >= 0xD800U && unit <= 0xDBFFU)
        {
            uint32_t low = 0U;
            (void)hex4(data, end, *position + 2U, &low);
            *position += 6U;
            return 0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U);
        }
        return unit;
    }
    default:
        return (uint32_t)(unsigned char)escape;
    }
}

bool spansEqual(const char *data, const Span &left, const Span &right)
{
    size_t a = left.offset, b = right.offset;
    const size_t aEnd = (size_t)left.offset + left.length, bEnd = (size_t)right.offset + right.length;
    while (a < aEnd && b < bEnd)
    {
        if (nextCharacter(data, aEnd, &a) != nextCharacter(data, bEnd, &b))
        {
            return false;
        }
    }
    return a == aEnd && b == bEnd;
}

bool spanEquals(const char *data, const Span &span, const char *ascii)
{
    size_t position = span.offset;
    const size_t end = (size_t)span.offset + span.length;
    for (; *ascii != '\0'; ascii++)
    {
        if (position >= end || nextCharacter(data, end, &position) != (uint32_t)(unsigned char)*ascii)
        {
            return false;
        }
    }
    return position == end;
}
} // namespace

NivaloJsonRule nivaloScanJson(const char *data, size_t length, unsigned maxDepth, unsigned maxValues,
                              NivaloJsonScan *result)
{
    NivaloJsonScan local;
    NivaloJsonScan &scan = result != NULL ? *result : local;
    const char *captureMember = scan.captureMember;
    scan = NivaloJsonScan();
    scan.captureMember = captureMember;
    if (data == NULL || length > 65535U || maxValues > NivaloJsonMaxValues || maxDepth > NivaloJsonMaxDepth)
    {
        scan.rule = NIVALO_JSON_SYNTAX;
        return scan.rule;
    }
    if (length >= 3U && (unsigned char)data[0] == 0xEFU && (unsigned char)data[1] == 0xBBU &&
        (unsigned char)data[2] == 0xBFU)
    {
        scan.rule = NIVALO_JSON_ENCODING;
        return scan.rule;
    }
    if (!nivaloIsUtf8(data, length))
    {
        scan.rule = NIVALO_JSON_ENCODING;
        return scan.rule;
    }

    Container stack[NivaloJsonMaxDepth];
    uint16_t keyStart[NivaloJsonMaxDepth];
    Span keys[NivaloJsonMaxValues];
    unsigned keyCount = 0U;
    unsigned depth = 0U;
    bool payloadPending = false;

    Scanner scanner(data, length);
    scanner.skipWhitespace();
    NivaloJsonRule rule = NIVALO_JSON_OK;

#define NIVALO_FAIL(r) \
    do                 \
    {                  \
        rule = (r);    \
        goto done;     \
    } while (0)

value:
    if (scanner.atEnd())
    {
        NIVALO_FAIL(NIVALO_JSON_SYNTAX);
    }
    if (++scan.values > maxValues)
    {
        NIVALO_FAIL(NIVALO_JSON_VALUE_COUNT);
    }
    {
        const char c = scanner.peek();
        if (c == '{' || c == '[')
        {
            if (depth + 1U > maxDepth)
            {
                NIVALO_FAIL(NIVALO_JSON_DEPTH);
            }
            if (depth == 0U)
            {
                scan.rootIsObject = c == '{';
            }
            stack[depth] = c == '{' ? CONTAINER_OBJECT : CONTAINER_ARRAY;
            keyStart[depth] = (uint16_t)keyCount;
            depth++;
            if (depth > scan.depth)
            {
                scan.depth = depth;
            }
            scanner.advance();
            scanner.skipWhitespace();
            if (c == '{')
            {
                if (scanner.peek() == '}')
                {
                    scanner.advance();
                    depth--;
                    goto afterValue;
                }
                goto member;
            }
            if (scanner.peek() == ']')
            {
                scanner.advance();
                depth--;
                goto afterValue;
            }
            goto value;
        }
        if (c == '"')
        {
            const NivaloJsonRule stringRule = scanner.string(NULL);
            if (stringRule != NIVALO_JSON_OK)
            {
                NIVALO_FAIL(stringRule);
            }
            goto afterValue;
        }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            if (!scanner.number())
            {
                NIVALO_FAIL(NIVALO_JSON_SYNTAX);
            }
            goto afterValue;
        }
        if ((c == 't' && scanner.literal("true")) || (c == 'f' && scanner.literal("false")) ||
            (c == 'n' && scanner.literal("null")))
        {
            goto afterValue;
        }
        NIVALO_FAIL(NIVALO_JSON_SYNTAX);
    }

member:
{
    Span key;
    const NivaloJsonRule keyRule = scanner.string(&key);
    if (keyRule != NIVALO_JSON_OK)
    {
        NIVALO_FAIL(keyRule);
    }
    for (unsigned i = keyStart[depth - 1U]; i < keyCount; i++)
    {
        if (spansEqual(data, keys[i], key))
        {
            NIVALO_FAIL(NIVALO_JSON_DUPLICATE_MEMBER);
        }
    }
    if (keyCount >= NivaloJsonMaxValues)
    {
        NIVALO_FAIL(NIVALO_JSON_VALUE_COUNT);
    }
    keys[keyCount++] = key;
    scanner.skipWhitespace();
    if (scanner.peek() != ':')
    {
        NIVALO_FAIL(NIVALO_JSON_SYNTAX);
    }
    scanner.advance();
    scanner.skipWhitespace();
    if (depth == 1U && scan.captureMember != NULL && spanEquals(data, key, scan.captureMember))
    {
        scan.hasPayload = true;
        scan.payloadOffset = scanner.position();
        payloadPending = true;
    }
    goto value;
}

afterValue:
    if (payloadPending && depth == 1U)
    {
        scan.payloadLength = scanner.position() - scan.payloadOffset;
        payloadPending = false;
    }
    scanner.skipWhitespace();
    if (depth == 0U)
    {
        if (!scanner.atEnd())
        {
            NIVALO_FAIL(NIVALO_JSON_SYNTAX);
        }
        goto done;
    }
    if (scanner.peek() == ',')
    {
        scanner.advance();
        scanner.skipWhitespace();
        if (stack[depth - 1U] == CONTAINER_OBJECT)
        {
            goto member;
        }
        goto value;
    }
    if ((stack[depth - 1U] == CONTAINER_OBJECT && scanner.peek() == '}') ||
        (stack[depth - 1U] == CONTAINER_ARRAY && scanner.peek() == ']'))
    {
        scanner.advance();
        keyCount = keyStart[depth - 1U];
        depth--;
        goto afterValue;
    }
    NIVALO_FAIL(NIVALO_JSON_SYNTAX);

done:
#undef NIVALO_FAIL
    scan.rule = rule;
    if (rule != NIVALO_JSON_OK)
    {
        scan.hasPayload = false;
    }
    return rule;
}
