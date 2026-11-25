#include "NivaloWire.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace
{
bool isDigit(char c) { return c >= '0' && c <= '9'; }

int digits(const char *value, size_t count)
{
    int result = 0;
    for (size_t i = 0U; i < count; i++)
    {
        if (!isDigit(value[i]))
        {
            return -1;
        }
        result = result * 10 + (value[i] - '0');
    }
    return result;
}

bool leapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysInMonth(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leapYear(year) ? 29 : days[month - 1];
}

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int year, int month, int day)
{
    year -= month <= 2 ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yearOfEra = year - era * 400;
    const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

void civilFromDays(int64_t days, int *year, int *month, int *day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t dayOfEra = days - era * 146097;
    const int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const int64_t monthPrime = (5 * dayOfYear + 2) / 153;
    *day = (int)(dayOfYear - (153 * monthPrime + 2) / 5 + 1);
    *month = (int)(monthPrime < 10 ? monthPrime + 3 : monthPrime - 9);
    *year = (int)(yearOfEra + era * 400 + (*month <= 2 ? 1 : 0));
}

const char HexDigits[] = "0123456789abcdef";
} // namespace

// --- UTF-8 -------------------------------------------------------------------------------------

static bool utf8Next(const uint8_t *s, size_t length, size_t *index, uint32_t *codePoint)
{
    size_t i = *index;
    const uint8_t lead = s[i];
    if (lead < 0x80U)
    {
        *codePoint = lead;
        *index = i + 1U;
        return true;
    }
    size_t count;
    uint32_t value;
    uint8_t low = 0x80U, high = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU) { count = 1U; value = lead & 0x1FU; }
    else if (lead >= 0xE0U && lead <= 0xEFU)
    {
        count = 2U; value = lead & 0x0FU;
        if (lead == 0xE0U) low = 0xA0U;
        if (lead == 0xEDU) high = 0x9FU;
    }
    else if (lead >= 0xF0U && lead <= 0xF4U)
    {
        count = 3U; value = lead & 0x07U;
        if (lead == 0xF0U) low = 0x90U;
        if (lead == 0xF4U) high = 0x8FU;
    }
    else
    {
        return false;
    }
    if (length - i <= count)
    {
        return false;
    }
    for (size_t k = 1U; k <= count; k++)
    {
        const uint8_t next = s[i + k];
        const uint8_t min = k == 1U ? low : 0x80U;
        const uint8_t max = k == 1U ? high : 0xBFU;
        if (next < min || next > max)
        {
            return false;
        }
        value = (value << 6U) | (next & 0x3FU);
    }
    *codePoint = value;
    *index = i + count + 1U;
    return true;
}

bool nivaloUtf8Next(const char *value, size_t length, size_t *index, uint32_t *codePoint)
{
    return value != NULL && index != NULL && codePoint != NULL && *index < length &&
           utf8Next(reinterpret_cast<const uint8_t *>(value), length, index, codePoint);
}

bool nivaloIsUtf8(const char *value, size_t length)
{
    if (value == NULL)
    {
        return false;
    }
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(value);
    size_t index = 0U;
    while (index < length)
    {
        uint32_t codePoint;
        if (!utf8Next(bytes, length, &index, &codePoint) || codePoint == 0U)
        {
            return false;
        }
    }
    return true;
}

// --- Text and identifiers ----------------------------------------------------------------------

bool nivaloIsUuid(const char *value, size_t length)
{
    if (value == NULL || length != NivaloV11::UuidLength)
    {
        return false;
    }
    bool nonZero = false;
    for (size_t i = 0U; i < length; i++)
    {
        const char c = value[i];
        if (i == 8U || i == 13U || i == 18U || i == 23U)
        {
            if (c != '-')
            {
                return false;
            }
            continue;
        }
        if (!(isDigit(c) || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
        nonZero = nonZero || c != '0';
    }
    return nonZero;
}

bool nivaloIsUuid(const char *value)
{
    return value != NULL && nivaloIsUuid(value, strlen(value));
}

bool nivaloIsName(const char *value)
{
    if (value == NULL)
    {
        return false;
    }
    const size_t length = strlen(value);
    if (length == 0U || length > NivaloV11::MaxNameLength)
    {
        return false;
    }
    const char first = value[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')))
    {
        return false;
    }
    for (size_t i = 1U; i < length; i++)
    {
        const char c = value[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || isDigit(c) || c == '_' || c == '.' || c == '-'))
        {
            return false;
        }
    }
    return true;
}

static bool asciiPrefixCaseEqual(const char *value, const char *prefix)
{
    for (; *prefix != '\0'; value++, prefix++)
    {
        char c = *value;
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
        if (c != *prefix)
        {
            return false;
        }
    }
    return true;
}

bool nivaloAsciiCaseEqual(const char *left, const char *right)
{
    if (left == NULL || right == NULL)
    {
        return false;
    }
    for (;; left++, right++)
    {
        char a = *left, b = *right;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
        {
            return false;
        }
        if (a == '\0')
        {
            return true;
        }
    }
}

bool nivaloIsReservedFunctionName(const char *value)
{
    if (value == NULL)
    {
        return false;
    }
    return nivaloAsciiCaseEqual(value, "flash") || nivaloAsciiCaseEqual(value, "requestdefinitions") ||
           asciiPrefixCaseEqual(value, "firmware.") || asciiPrefixCaseEqual(value, "ota.");
}

bool nivaloIsFunctionName(const char *value)
{
    return nivaloIsName(value) && !nivaloIsReservedFunctionName(value);
}

bool nivaloIsText(const char *value, size_t maxBytes)
{
    if (value == NULL)
    {
        return false;
    }
    const size_t length = strlen(value);
    if (length == 0U || length > maxBytes || !nivaloIsUtf8(value, length))
    {
        return false;
    }
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(value);
    size_t index = 0U;
    uint32_t codePoint = 0U;
    bool first = true;
    while (index < length)
    {
        (void)utf8Next(bytes, length, &index, &codePoint);
        if (codePoint < 0x20U || codePoint == 0x7FU)
        {
            return false;
        }
        if (first && codePoint == 0x20U)
        {
            return false;
        }
        first = false;
    }
    return codePoint != 0x20U;
}

bool nivaloIsLowerHex(const char *value, size_t length)
{
    if (value == NULL || strlen(value) != length)
    {
        return false;
    }
    for (size_t i = 0U; i < length; i++)
    {
        if (!(isDigit(value[i]) || (value[i] >= 'a' && value[i] <= 'f')))
        {
            return false;
        }
    }
    return true;
}

// --- Time --------------------------------------------------------------------------------------

bool nivaloParseUtc(const char *value, int64_t *epochMs)
{
    if (value == NULL)
    {
        return false;
    }
    const size_t length = strlen(value);
    if (length != 20U && length != 24U)
    {
        return false;
    }
    if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[length - 1U] != 'Z')
    {
        return false;
    }
    const int year = digits(value, 4U);
    const int month = digits(value + 5, 2U);
    const int day = digits(value + 8, 2U);
    const int hour = digits(value + 11, 2U);
    const int minute = digits(value + 14, 2U);
    const int second = digits(value + 17, 2U);
    int millis = 0;
    if (length == 24U)
    {
        if (value[19] != '.')
        {
            return false;
        }
        millis = digits(value + 20, 3U);
    }
    if (year < 2020 || year > 2037 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59 || millis < 0)
    {
        return false;
    }
    if (day > daysInMonth(year, month))
    {
        return false;
    }
    if (epochMs != NULL)
    {
        const int64_t days = daysFromCivil(year, month, day);
        *epochMs = ((days * 24 + hour) * 60 + minute) * 60000LL + second * 1000LL + millis;
    }
    return true;
}

bool nivaloFormatUtc(int64_t epochMs, char *output, size_t capacity)
{
    if (output == NULL || capacity < NivaloV11::UtcCapacity || epochMs < 0)
    {
        return false;
    }
    const int64_t days = epochMs / 86400000LL;
    const int64_t msOfDay = epochMs % 86400000LL;
    int year, month, day;
    civilFromDays(days, &year, &month, &day);
    if (year < 2020 || year > 2037)
    {
        return false;
    }
    const int hour = (int)(msOfDay / 3600000LL);
    const int minute = (int)((msOfDay / 60000LL) % 60);
    const int second = (int)((msOfDay / 1000LL) % 60);
    const int millis = (int)(msOfDay % 1000LL);
    snprintf(output, capacity, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", year, month, day, hour, minute, second, millis);
    return true;
}

// --- Identifiers -------------------------------------------------------------------------------

void nivaloFormatUuidV4(const uint8_t random[16], char output[37])
{
    uint8_t bytes[16];
    memcpy(bytes, random, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = (uint8_t)((bytes[8] & 0x3FU) | 0x80U);
    size_t position = 0U;
    for (size_t i = 0U; i < 16U; i++)
    {
        if (i == 4U || i == 6U || i == 8U || i == 10U)
        {
            output[position++] = '-';
        }
        output[position++] = HexDigits[bytes[i] >> 4U];
        output[position++] = HexDigits[bytes[i] & 0x0FU];
    }
    output[position] = '\0';
}

bool nivaloSeqMatchesId(uint32_t seq, const char *operationId)
{
    if (!nivaloIsUuid(operationId))
    {
        return false;
    }
    char expected[9];
    snprintf(expected, sizeof(expected), "%08lx", (unsigned long)seq);
    return memcmp(operationId + 28, expected, 8U) == 0;
}

// --- Decimal -----------------------------------------------------------------------------------

bool nivaloFormatDecimal(double value, char *output, size_t capacity)
{
    if (output == NULL || capacity < 2U || !isfinite(value) || fabs(value) >= 4294967296.0)
    {
        return false;
    }
    char text[48];
    const int written = snprintf(text, sizeof(text), "%.6f", value);
    if (written <= 0 || (size_t)written >= sizeof(text))
    {
        return false;
    }
    size_t length = (size_t)written;
    char *point = strchr(text, '.');
    if (point != NULL)
    {
        while (length > 0U && text[length - 1U] == '0')
        {
            text[--length] = '\0';
        }
        if (length > 0U && text[length - 1U] == '.')
        {
            text[--length] = '\0';
        }
    }
    const char *integerPart = text[0] == '-' ? text + 1 : text;
    const size_t integerDigits = strcspn(integerPart, ".");
    if (integerDigits > 10U || (integerDigits == 10U && strncmp(integerPart, "4294967295", 10U) > 0))
    {
        return false;
    }
    if (strcmp(text, "-0") == 0)
    {
        strcpy(text, "0");
        length = 1U;
    }
    if (length + 1U > capacity)
    {
        return false;
    }
    memcpy(output, text, length + 1U);
    return true;
}

// --- JSON writer -------------------------------------------------------------------------------

NivaloJsonWriter::NivaloJsonWriter(char *buffer, size_t capacity) : _buffer(buffer), _capacity(capacity)
{
    if (_buffer == NULL || _capacity == 0U)
    {
        _failed = true;
        return;
    }
    _buffer[0] = '\0';
}

void NivaloJsonWriter::put(char c)
{
    if (_failed || _length + 1U >= _capacity)
    {
        _failed = true;
        return;
    }
    _buffer[_length++] = c;
    _buffer[_length] = '\0';
}

void NivaloJsonWriter::put(const char *text, size_t length)
{
    if (_failed || length >= _capacity - _length)
    {
        _failed = true;
        return;
    }
    memcpy(_buffer + _length, text, length);
    _length += length;
    _buffer[_length] = '\0';
}

void NivaloJsonWriter::separator()
{
    if (_afterKey)
    {
        _afterKey = false;
        return;
    }
    if (_needComma)
    {
        put(',');
    }
}

void NivaloJsonWriter::beginObject()
{
    separator();
    put('{');
    _needComma = false;
    _depth++;
}

void NivaloJsonWriter::endObject()
{
    if (_depth == 0U || _afterKey)
    {
        _failed = true;
        return;
    }
    put('}');
    _depth--;
    _needComma = true;
}

void NivaloJsonWriter::beginArray()
{
    separator();
    put('[');
    _needComma = false;
    _depth++;
}

void NivaloJsonWriter::endArray()
{
    if (_depth == 0U || _afterKey)
    {
        _failed = true;
        return;
    }
    put(']');
    _depth--;
    _needComma = true;
}

void NivaloJsonWriter::key(const char *name)
{
    if (_afterKey || name == NULL)
    {
        _failed = true;
        return;
    }
    separator();
    quoted(name, strlen(name));
    put(':');
    _afterKey = true;
    _needComma = false;
}

void NivaloJsonWriter::string(const char *value)
{
    string(value, value == NULL ? 0U : strlen(value));
}

void NivaloJsonWriter::string(const char *value, size_t length)
{
    if (value == NULL)
    {
        _failed = true;
        return;
    }
    separator();
    quoted(value, length);
    _needComma = true;
}

void NivaloJsonWriter::quoted(const char *value, size_t length)
{
    put('"');
    for (size_t i = 0U; i < length; i++)
    {
        const unsigned char c = (unsigned char)value[i];
        switch (c)
        {
        case '"': put("\\\"", 2U); break;
        case '\\': put("\\\\", 2U); break;
        case '\b': put("\\b", 2U); break;
        case '\f': put("\\f", 2U); break;
        case '\n': put("\\n", 2U); break;
        case '\r': put("\\r", 2U); break;
        case '\t': put("\\t", 2U); break;
        default:
            if (c < 0x20U)
            {
                char escape[7];
                snprintf(escape, sizeof(escape), "\\u%04x", (unsigned)c);
                put(escape, 6U);
            }
            else
            {
                put((char)c);
            }
        }
    }
    put('"');
}

void NivaloJsonWriter::uint(uint64_t value)
{
    separator();
    char text[24];
    const int written = snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    put(text, (size_t)written);
    _needComma = true;
}

void NivaloJsonWriter::integer(int64_t value)
{
    separator();
    char text[24];
    const int written = snprintf(text, sizeof(text), "%lld", (long long)value);
    put(text, (size_t)written);
    _needComma = true;
}

void NivaloJsonWriter::boolean(bool value)
{
    separator();
    if (value)
    {
        put("true", 4U);
    }
    else
    {
        put("false", 5U);
    }
    _needComma = true;
}

void NivaloJsonWriter::raw(const char *json, size_t length)
{
    if (json == NULL || length == 0U)
    {
        _failed = true;
        return;
    }
    separator();
    put(json, length);
    _needComma = true;
}

bool NivaloJsonWriter::decimal(double value)
{
    char text[32];
    if (!nivaloFormatDecimal(value, text, sizeof(text)))
    {
        _failed = true;
        return false;
    }
    separator();
    put(text, strlen(text));
    _needComma = true;
    return true;
}

// --- Topics ------------------------------------------------------------------------------------

bool NivaloTopics::configure(const char *deviceId)
{
    if (!nivaloIsUuid(deviceId))
    {
        return false;
    }
    memcpy(_deviceId, deviceId, 37U);
    snprintf(_prefix, sizeof(_prefix), "%s%s/", NivaloV11::TopicRoot, deviceId);
    snprintf(_telemetry, sizeof(_telemetry), "%stelemetry", _prefix);
    snprintf(_events, sizeof(_events), "%sevents", _prefix);
    snprintf(_state, sizeof(_state), "%sstate", _prefix);
    snprintf(_definitions, sizeof(_definitions), "%sdefinitions", _prefix);
    snprintf(_availability, sizeof(_availability), "%savailability", _prefix);
    snprintf(_commandsFilter, sizeof(_commandsFilter), "%scommands/+", _prefix);
    snprintf(_otaFilter, sizeof(_otaFilter), "%sota/+", _prefix);
    return true;
}

bool NivaloTopics::ack(NivaloOperationClass operationClass, const char *operationId, char *output, size_t capacity) const
{
    if (!nivaloIsUuid(operationId) || _deviceId[0] == '\0' || output == NULL)
    {
        return false;
    }
    const int written = snprintf(output, capacity, "%s%s/%s/ack", _prefix,
                                 operationClass == NIVALO_OPERATION_OTA ? "ota" : "commands", operationId);
    return written > 0 && (size_t)written < capacity;
}

bool NivaloTopics::parseDownlink(const char *topic, NivaloOperationClass *operationClass, char operationId[37]) const
{
    if (topic == NULL || _deviceId[0] == '\0')
    {
        return false;
    }
    const size_t prefixLength = strlen(_prefix);
    if (strncmp(topic, _prefix, prefixLength) != 0)
    {
        return false;
    }
    const char *route = topic + prefixLength;
    NivaloOperationClass parsedClass;
    const char *id;
    if (strncmp(route, "commands/", 9U) == 0)
    {
        parsedClass = NIVALO_OPERATION_COMMAND;
        id = route + 9;
    }
    else if (strncmp(route, "ota/", 4U) == 0)
    {
        parsedClass = NIVALO_OPERATION_OTA;
        id = route + 4;
    }
    else
    {
        return false;
    }
    if (!nivaloIsUuid(id))
    {
        return false;
    }
    if (operationClass != NULL)
    {
        *operationClass = parsedClass;
    }
    if (operationId != NULL)
    {
        memcpy(operationId, id, 37U);
    }
    return true;
}
