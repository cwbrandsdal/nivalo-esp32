// Host stand-in for Arduino-ESP32's Preferences (NVS), enough for NivaloDeviceStore. The storage
// is process-wide and survives Preferences objects, like flash survives a reset. Tests reach it
// through FakeNvs to inspect it, damage records and inject read failures.
#ifndef NIVALO_HOST_FAKE_PREFERENCES_H
#define NIVALO_HOST_FAKE_PREFERENCES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

struct FakeNvs
{
    struct Entry
    {
        bool isBlob = false;
        uint32_t number = 0U;
        std::vector<uint8_t> bytes;
    };
    std::map<std::string, Entry> entries; // "namespace/key"
    // Keys whose reads fail (the key is present, the read returns nothing).
    std::set<std::string> unreadable;
    // Every write, as "namespace/key".
    std::vector<std::string> writes;

    static FakeNvs &instance()
    {
        static FakeNvs nvs;
        return nvs;
    }
    void reset() { *this = FakeNvs(); }
};

class Preferences
{
public:
    bool begin(const char *name, bool readOnly = false)
    {
        _namespace = name == NULL ? "" : name;
        _readOnly = readOnly;
        _open = true;
        return true;
    }
    void end() { _open = false; }

    bool isKey(const char *key) const { return _open && nvs().entries.count(path(key)) > 0U; }
    bool remove(const char *key)
    {
        if (!_open || _readOnly)
        {
            return false;
        }
        nvs().writes.push_back(path(key));
        return nvs().entries.erase(path(key)) > 0U;
    }

    size_t putBytes(const char *key, const void *value, size_t length)
    {
        if (!_open || _readOnly)
        {
            return 0U;
        }
        FakeNvs::Entry entry;
        entry.isBlob = true;
        entry.bytes.assign(static_cast<const uint8_t *>(value), static_cast<const uint8_t *>(value) + length);
        nvs().entries[path(key)] = entry;
        nvs().writes.push_back(path(key));
        return length;
    }
    size_t getBytesLength(const char *key) const
    {
        const FakeNvs::Entry *entry = find(key);
        return entry == NULL || !entry->isBlob || readFails(key) ? 0U : entry->bytes.size();
    }
    size_t getBytes(const char *key, void *buffer, size_t capacity) const
    {
        const FakeNvs::Entry *entry = find(key);
        if (entry == NULL || !entry->isBlob || readFails(key) || entry->bytes.size() > capacity)
        {
            return 0U;
        }
        memcpy(buffer, entry->bytes.data(), entry->bytes.size());
        return entry->bytes.size();
    }

    size_t putULong(const char *key, uint32_t value)
    {
        if (!_open || _readOnly)
        {
            return 0U;
        }
        FakeNvs::Entry entry;
        entry.number = value;
        nvs().entries[path(key)] = entry;
        nvs().writes.push_back(path(key));
        return sizeof(uint32_t);
    }
    uint32_t getULong(const char *key, uint32_t fallback = 0U) const
    {
        const FakeNvs::Entry *entry = find(key);
        return entry == NULL || entry->isBlob || readFails(key) ? fallback : entry->number;
    }

private:
    static FakeNvs &nvs() { return FakeNvs::instance(); }
    std::string path(const char *key) const { return _namespace + "/" + key; }
    const FakeNvs::Entry *find(const char *key) const
    {
        if (!_open)
        {
            return NULL;
        }
        std::map<std::string, FakeNvs::Entry>::const_iterator found = nvs().entries.find(path(key));
        return found == nvs().entries.end() ? NULL : &found->second;
    }
    bool readFails(const char *key) const { return nvs().unreadable.count(path(key)) > 0U; }

    std::string _namespace;
    bool _readOnly = false;
    bool _open = false;
};

#endif
