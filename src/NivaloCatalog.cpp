#include "NivaloCatalog.h"

#include <new>
#include <string.h>

#include <mbedtls/sha256.h>

#include "NivaloJournal.h"

namespace
{
// One upsert at its largest: a 64-byte name and a function definition whose displayName (128
// bytes), description (256) and argumentExample (256) all double under JSON escaping, plus the
// fixed members, is about 1550 bytes.
const size_t OpCapacity = 2048U;
const size_t MaxOpsPerPacket = 8U;

const char *kindName(uint8_t kind) { return kind == NIVALO_KIND_FUNCTION ? "function" : "variable"; }

bool sameName(const NivaloCatalogName &entry, uint8_t kind, const char *name)
{
    return entry.kind == kind && strcmp(entry.name, name) == 0;
}

bool isLive(const NivaloCatalogState &state, uint8_t kind, const char *name)
{
    for (size_t i = 0U; i < state.liveCount; i++)
    {
        if (sameName(state.live[i], kind, name))
        {
            return true;
        }
    }
    return false;
}

// Every retirement stays: dropping one before it was reported would leave the name live on the
// server, where absence means no change. False when the budget is exhausted.
bool appendRetired(NivaloCatalogState &state, const NivaloCatalogName &entry)
{
    if (state.retiredCount == NivaloCatalogMaxRetired)
    {
        return false;
    }
    state.retired[state.retiredCount++] = entry;
    return true;
}

// The refresh pairs a live name with its retired case twin in one packet (section 12.4). With two
// retired twins the pairing is ambiguous and the server can reject the packet (name-collision).
bool ambiguousCaseRename(const NivaloCatalogState &state)
{
    for (size_t i = 0U; i < state.liveCount; i++)
    {
        unsigned twins = 0U;
        for (size_t r = 0U; r < state.retiredCount; r++)
        {
            if (state.retired[r].kind == state.live[i].kind &&
                nivaloAsciiCaseEqual(state.retired[r].name, state.live[i].name))
            {
                twins++;
            }
        }
        if (twins > 1U)
        {
            return true;
        }
    }
    return false;
}

bool writeUpsert(const NivaloCatalogSource &source, size_t index, char *output, size_t capacity, size_t *length)
{
    NivaloJsonWriter writer(output, capacity);
    writer.beginObject();
    writer.key("op");
    writer.string("upsert");
    writer.key("kind");
    writer.string(kindName((uint8_t)source.capabilityKind(index)));
    writer.key("name");
    writer.string(source.capabilityName(index));
    writer.key("definition");
    source.writeDefinition(index, writer);
    writer.endObject();
    *length = writer.length();
    return !writer.failed();
}

bool writeRemove(const NivaloCatalogName &entry, char *output, size_t capacity, size_t *length)
{
    NivaloJsonWriter writer(output, capacity);
    writer.beginObject();
    writer.key("op");
    writer.string("remove");
    writer.key("kind");
    writer.string(kindName(entry.kind));
    writer.key("name");
    writer.string(entry.name);
    writer.endObject();
    *length = writer.length();
    return !writer.failed();
}

class Packer
{
public:
    Packer(char *buffer, size_t capacity, uint32_t revision, NivaloCatalogSink &sink)
        : _buffer(buffer), _capacity(capacity), _revision(revision), _sink(sink)
    {
        start();
    }

    // Adds a group of operations that must share one packet.
    bool group(char *const *ops, const size_t *lengths, size_t count)
    {
        size_t bytes = 0U;
        for (size_t i = 0U; i < count; i++)
        {
            bytes += lengths[i] + 1U;
        }
        if (_ops > 0U && (_ops + count > MaxOpsPerPacket || _length + bytes + 2U >= _capacity))
        {
            if (!flush())
            {
                return false;
            }
        }
        if (count > MaxOpsPerPacket || _length + bytes + 2U >= _capacity)
        {
            return false;
        }
        for (size_t i = 0U; i < count; i++)
        {
            if (_ops > 0U)
            {
                _buffer[_length++] = ',';
            }
            memcpy(_buffer + _length, ops[i], lengths[i]);
            _length += lengths[i];
            _ops++;
        }
        return true;
    }

    bool flush()
    {
        if (_ops == 0U)
        {
            return true;
        }
        _buffer[_length++] = ']';
        _buffer[_length++] = '}';
        _buffer[_length] = '\0';
        const bool emitted = _sink.emitPacket(_buffer, _length);
        start();
        return emitted;
    }

private:
    void start()
    {
        NivaloJsonWriter writer(_buffer, _capacity);
        writer.beginObject();
        writer.key("source");
        writer.string("esp32");
        writer.key("revision");
        writer.uint(_revision);
        writer.key("ops");
        _length = writer.length();
        _buffer[_length++] = '[';
        _buffer[_length] = '\0';
        _ops = 0U;
    }

    char *_buffer;
    size_t _capacity;
    uint32_t _revision;
    NivaloCatalogSink &_sink;
    size_t _length = 0U;
    size_t _ops = 0U;
};
} // namespace

NivaloCatalogDecision nivaloPlanCatalog(const NivaloCatalogSource &source, const NivaloCatalogState *stored,
                                        NivaloCatalogState *next)
{
    if (next == NULL)
    {
        return NIVALO_CATALOG_INVALID;
    }
    const size_t count = source.capabilityCount();
    if (count > NivaloCatalogMaxLive)
    {
        return NIVALO_CATALOG_INVALID;
    }
    // Digest over the live entries ordered by kind, then name.
    size_t order[NivaloCatalogMaxLive];
    for (size_t i = 0U; i < count; i++)
    {
        size_t position = i;
        while (position > 0U)
        {
            const size_t previous = order[position - 1U];
            const int byKind = (int)source.capabilityKind(previous) - (int)source.capabilityKind(i);
            if (byKind < 0 || (byKind == 0 && strcmp(source.capabilityName(previous), source.capabilityName(i)) < 0))
            {
                break;
            }
            order[position] = previous;
            position--;
        }
        order[position] = i;
    }
    char *definition = new (std::nothrow) char[OpCapacity];
    if (definition == NULL)
    {
        return NIVALO_CATALOG_INVALID;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts_ret(&sha, 0) == 0;
    for (size_t k = 0U; ok && k < count; k++)
    {
        const size_t i = order[k];
        const uint8_t kind = (uint8_t)source.capabilityKind(i);
        const char *name = source.capabilityName(i);
        NivaloJsonWriter writer(definition, OpCapacity);
        source.writeDefinition(i, writer);
        const uint8_t zero = 0U;
        ok = !writer.failed() && nivaloIsName(name) && mbedtls_sha256_update_ret(&sha, &kind, 1U) == 0 &&
             mbedtls_sha256_update_ret(&sha, reinterpret_cast<const uint8_t *>(name), strlen(name)) == 0 &&
             mbedtls_sha256_update_ret(&sha, &zero, 1U) == 0 &&
             mbedtls_sha256_update_ret(&sha, reinterpret_cast<const uint8_t *>(writer.c_str()), writer.length()) == 0 &&
             mbedtls_sha256_update_ret(&sha, &zero, 1U) == 0;
    }
    uint8_t digest[32];
    ok = ok && mbedtls_sha256_finish_ret(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    delete[] definition;
    if (!ok)
    {
        return NIVALO_CATALOG_INVALID;
    }

    if (stored != NULL && memcmp(stored->digest, digest, sizeof(digest)) == 0)
    {
        *next = *stored;
        return NIVALO_CATALOG_UNCHANGED;
    }
    if (stored != NULL && stored->revision >= NivaloCatalogMaxRevision)
    {
        return NIVALO_CATALOG_INVALID;
    }
    NivaloCatalogState state;
    state.revision = stored == NULL ? 1U : stored->revision + 1U;
    memcpy(state.digest, digest, sizeof(digest));
    for (size_t i = 0U; i < count; i++)
    {
        state.live[i].kind = (uint8_t)source.capabilityKind(i);
        memcpy(state.live[i].name, source.capabilityName(i), strlen(source.capabilityName(i)) + 1U);
    }
    state.liveCount = (uint8_t)count;
    if (stored != NULL)
    {
        for (size_t i = 0U; i < stored->retiredCount; i++)
        {
            if (!isLive(state, stored->retired[i].kind, stored->retired[i].name) &&
                !appendRetired(state, stored->retired[i]))
            {
                return NIVALO_CATALOG_REFUSED;
            }
        }
        for (size_t i = 0U; i < stored->liveCount; i++)
        {
            if (!isLive(state, stored->live[i].kind, stored->live[i].name) && !appendRetired(state, stored->live[i]))
            {
                return NIVALO_CATALOG_REFUSED;
            }
        }
    }
    if (ambiguousCaseRename(state))
    {
        return NIVALO_CATALOG_REFUSED;
    }
    *next = state;
    return NIVALO_CATALOG_CHANGED;
}

size_t nivaloEncodeCatalog(const NivaloCatalogState &state, uint8_t *output, size_t capacity)
{
    if (output == NULL || state.liveCount > NivaloCatalogMaxLive || state.retiredCount > NivaloCatalogMaxRetired)
    {
        return 0U;
    }
    size_t length = 0U;
    bool failed = false;
    auto put = [&](uint8_t value) {
        if (length >= capacity)
        {
            failed = true;
            return;
        }
        output[length++] = value;
    };
    auto putName = [&](const NivaloCatalogName &entry) {
        const size_t nameLength = strnlen(entry.name, 64U);
        put(entry.kind);
        put((uint8_t)nameLength);
        for (size_t i = 0U; i < nameLength; i++)
        {
            put((uint8_t)entry.name[i]);
        }
    };
    put('N');
    put('C');
    put(1U);
    for (int i = 0; i < 4; i++)
    {
        put((uint8_t)(state.revision >> (8 * i)));
    }
    for (size_t i = 0U; i < sizeof(state.digest); i++)
    {
        put(state.digest[i]);
    }
    put(state.liveCount);
    for (size_t i = 0U; i < state.liveCount; i++)
    {
        putName(state.live[i]);
    }
    put(state.retiredCount);
    for (size_t i = 0U; i < state.retiredCount; i++)
    {
        putName(state.retired[i]);
    }
    const uint32_t crc = nivaloCrc32(output, failed ? 0U : length);
    for (int i = 0; i < 4; i++)
    {
        put((uint8_t)(crc >> (8 * i)));
    }
    return failed ? 0U : length;
}

bool nivaloDecodeCatalog(const uint8_t *data, size_t length, NivaloCatalogState *state)
{
    if (data == NULL || state == NULL || length < 3U + 4U + 32U + 2U + 4U || data[0] != 'N' || data[1] != 'C' ||
        data[2] != 1U)
    {
        return false;
    }
    const uint32_t stored = (uint32_t)data[length - 4U] | ((uint32_t)data[length - 3U] << 8U) |
                            ((uint32_t)data[length - 2U] << 16U) | ((uint32_t)data[length - 1U] << 24U);
    if (stored != nivaloCrc32(data, length - 4U))
    {
        return false;
    }
    const size_t end = length - 4U;
    size_t position = 3U;
    NivaloCatalogState decoded;
    for (int i = 0; i < 4; i++)
    {
        decoded.revision |= (uint32_t)data[position++] << (8 * i);
    }
    memcpy(decoded.digest, data + position, sizeof(decoded.digest));
    position += sizeof(decoded.digest);
    auto readNames = [&](NivaloCatalogName *names, uint8_t *count, size_t maximum) -> bool {
        if (position >= end)
        {
            return false;
        }
        *count = data[position++];
        if (*count > maximum)
        {
            return false;
        }
        for (size_t i = 0U; i < *count; i++)
        {
            if (end - position < 2U)
            {
                return false;
            }
            names[i].kind = data[position++];
            const size_t nameLength = data[position++];
            if (names[i].kind > NIVALO_KIND_FUNCTION || nameLength == 0U || nameLength > 64U || end - position < nameLength)
            {
                return false;
            }
            memcpy(names[i].name, data + position, nameLength);
            names[i].name[nameLength] = '\0';
            position += nameLength;
            if (!nivaloIsName(names[i].name))
            {
                return false;
            }
        }
        return true;
    };
    if (!readNames(decoded.live, &decoded.liveCount, NivaloCatalogMaxLive) ||
        !readNames(decoded.retired, &decoded.retiredCount, NivaloCatalogMaxRetired) || position != end ||
        decoded.revision == 0U || decoded.revision > NivaloCatalogMaxRevision)
    {
        return false;
    }
    *state = decoded;
    return true;
}

bool nivaloRefreshCatalog(const NivaloCatalogSource &source, const NivaloCatalogState &state,
                          size_t maxPayloadBytes, NivaloCatalogSink &sink)
{
    const size_t count = source.capabilityCount();
    if (count > NivaloCatalogMaxLive || state.revision == 0U || maxPayloadBytes < 64U)
    {
        return false;
    }
    char *packet = new (std::nothrow) char[maxPayloadBytes + 1U];
    char *opBuffers = new (std::nothrow) char[OpCapacity * 2U];
    if (packet == NULL || opBuffers == NULL)
    {
        delete[] packet;
        delete[] opBuffers;
        return false;
    }
    Packer packer(packet, maxPayloadBytes + 1U, state.revision, sink);
    bool used[NivaloCatalogMaxRetired] = {false};
    bool ok = true;
    char *ops[2] = {opBuffers, opBuffers + OpCapacity};
    size_t lengths[2] = {0U, 0U};
    for (size_t i = 0U; ok && i < count; i++)
    {
        // A live entry and the tombstone of its case twin form a case-only rename: one packet.
        size_t groupSize = 0U;
        const uint8_t kind = (uint8_t)source.capabilityKind(i);
        for (size_t r = 0U; ok && groupSize == 0U && r < state.retiredCount; r++)
        {
            if (!used[r] && state.retired[r].kind == kind && nivaloAsciiCaseEqual(state.retired[r].name, source.capabilityName(i)))
            {
                ok = writeRemove(state.retired[r], ops[0], OpCapacity, &lengths[0]);
                used[r] = true;
                groupSize = 1U;
            }
        }
        ok = ok && writeUpsert(source, i, ops[groupSize], OpCapacity, &lengths[groupSize]);
        ok = ok && packer.group(ops, lengths, groupSize + 1U);
    }
    for (size_t r = 0U; ok && r < state.retiredCount; r++)
    {
        if (used[r])
        {
            continue;
        }
        ops[0] = opBuffers;
        ok = writeRemove(state.retired[r], ops[0], OpCapacity, &lengths[0]) && packer.group(ops, lengths, 1U);
    }
    ok = ok && packer.flush();
    delete[] packet;
    delete[] opBuffers;
    return ok;
}
