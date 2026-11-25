// Host stand-in for ESP-IDF's native NVS API (nvs.h), enough for NivaloDeviceStore. The storage is
// process-wide and survives handles, like flash survives a reset. Tests reach it through FakeNvs
// to inspect it, damage records and inject read failures.
#ifndef NIVALO_HOST_FAKE_NVS_H
#define NIVALO_HOST_FAKE_NVS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_INVALID_HANDLE (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0c)

typedef uint32_t nvs_handle_t;
typedef enum
{
    NVS_READONLY,
    NVS_READWRITE
} nvs_open_mode_t;

struct FakeNvs
{
    struct Entry
    {
        bool isBlob = false;
        uint32_t number = 0U;
        std::vector<uint8_t> bytes;
    };
    std::map<std::string, Entry> entries; // "namespace/key"
    // Keys whose lookup itself fails with this error (the size/type lookup, before any data).
    std::map<std::string, esp_err_t> lookupErrors;
    // Keys whose data read fails after a successful lookup.
    std::set<std::string> unreadable;
    // Every write, as "namespace/key".
    std::vector<std::string> writes;
    std::map<nvs_handle_t, std::string> handles;
    nvs_handle_t nextHandle = 1U;

    static FakeNvs &instance()
    {
        static FakeNvs nvs;
        return nvs;
    }
    void reset() { *this = FakeNvs(); }

    bool path(nvs_handle_t handle, const char *key, std::string *out) const
    {
        std::map<nvs_handle_t, std::string>::const_iterator found = handles.find(handle);
        if (found == handles.end() || key == NULL)
        {
            return false;
        }
        *out = found->second + "/" + key;
        return true;
    }
    esp_err_t lookup(nvs_handle_t handle, const char *key, bool blob, const Entry **entry, std::string *where) const
    {
        if (!path(handle, key, where))
        {
            return ESP_ERR_NVS_INVALID_HANDLE;
        }
        std::map<std::string, esp_err_t>::const_iterator error = lookupErrors.find(*where);
        if (error != lookupErrors.end())
        {
            return error->second;
        }
        std::map<std::string, Entry>::const_iterator found = entries.find(*where);
        // NVS looks a key up together with its type: another type is not found.
        if (found == entries.end() || found->second.isBlob != blob)
        {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        *entry = &found->second;
        return ESP_OK;
    }
};

inline esp_err_t nvs_open(const char *name, nvs_open_mode_t, nvs_handle_t *handle)
{
    FakeNvs &nvs = FakeNvs::instance();
    *handle = nvs.nextHandle++;
    nvs.handles[*handle] = name == NULL ? "" : name;
    return ESP_OK;
}

inline void nvs_close(nvs_handle_t handle) { FakeNvs::instance().handles.erase(handle); }

inline esp_err_t nvs_commit(nvs_handle_t handle)
{
    return FakeNvs::instance().handles.count(handle) > 0U ? ESP_OK : ESP_ERR_NVS_INVALID_HANDLE;
}

inline esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    const FakeNvs &nvs = FakeNvs::instance();
    const FakeNvs::Entry *entry = NULL;
    std::string where;
    const esp_err_t error = nvs.lookup(handle, key, true, &entry, &where);
    if (error != ESP_OK)
    {
        return error;
    }
    if (out == NULL)
    {
        *length = entry->bytes.size();
        return ESP_OK;
    }
    if (nvs.unreadable.count(where) > 0U)
    {
        return ESP_FAIL;
    }
    if (*length < entry->bytes.size())
    {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (!entry->bytes.empty())
    {
        memcpy(out, entry->bytes.data(), entry->bytes.size());
    }
    *length = entry->bytes.size();
    return ESP_OK;
}

inline esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    FakeNvs &nvs = FakeNvs::instance();
    std::string where;
    if (!nvs.path(handle, key, &where))
    {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    FakeNvs::Entry entry;
    entry.isBlob = true;
    entry.bytes.assign(static_cast<const uint8_t *>(value), static_cast<const uint8_t *>(value) + length);
    nvs.entries[where] = entry;
    nvs.writes.push_back(where);
    return ESP_OK;
}

inline esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value)
{
    const FakeNvs &nvs = FakeNvs::instance();
    const FakeNvs::Entry *entry = NULL;
    std::string where;
    const esp_err_t error = nvs.lookup(handle, key, false, &entry, &where);
    if (error != ESP_OK)
    {
        return error;
    }
    if (nvs.unreadable.count(where) > 0U)
    {
        return ESP_FAIL;
    }
    *value = entry->number;
    return ESP_OK;
}

inline esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    FakeNvs &nvs = FakeNvs::instance();
    std::string where;
    if (!nvs.path(handle, key, &where))
    {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    FakeNvs::Entry entry;
    entry.number = value;
    nvs.entries[where] = entry;
    nvs.writes.push_back(where);
    return ESP_OK;
}

inline esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    FakeNvs &nvs = FakeNvs::instance();
    std::string where;
    if (!nvs.path(handle, key, &where))
    {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    nvs.writes.push_back(where);
    return nvs.entries.erase(where) > 0U ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

#endif
