#include "NivaloDeviceStore.h"

#include <new>
#include <string.h>

namespace
{
const char Namespace[] = "nivalo-dev";
const char BootKey[] = "boot";
const char JournalKey[] = "journal";
const char PendingKey[] = "otaPending";
const char FloorKey[] = "otaFloor";
const char CatalogKey[] = "catalog";

NivaloStoreRead readStatus(esp_err_t error)
{
    if (error == ESP_OK)
    {
        return NIVALO_STORE_OK;
    }
    // Only a genuine "not found" is absence; a failed lookup is not.
    return error == ESP_ERR_NVS_NOT_FOUND ? NIVALO_STORE_MISSING : NIVALO_STORE_UNREADABLE;
}
} // namespace

bool NivaloDeviceStore::begin()
{
    if (!_opened)
    {
        _opened = nvs_open(Namespace, NVS_READWRITE, &_handle) == ESP_OK;
    }
    return _opened;
}

void NivaloDeviceStore::end()
{
    if (_opened)
    {
        nvs_close(_handle);
        _opened = false;
    }
}

bool NivaloDeviceStore::commit()
{
    return nvs_commit(_handle) == ESP_OK;
}

bool NivaloDeviceStore::writeBlob(const char *key, const uint8_t *data, size_t length)
{
    if (!_opened || data == NULL || length == 0U)
    {
        return false;
    }
    if (nvs_set_blob(_handle, key, data, length) != ESP_OK || !commit())
    {
        return false;
    }
    uint8_t *check = new (std::nothrow) uint8_t[length];
    if (check == NULL)
    {
        return false;
    }
    size_t stored = 0U;
    const bool same = readBlob(key, check, length, &stored) == NIVALO_STORE_OK && stored == length &&
                      memcmp(check, data, length) == 0;
    delete[] check;
    return same;
}

NivaloStoreRead NivaloDeviceStore::readBlob(const char *key, uint8_t *data, size_t capacity, size_t *length)
{
    *length = 0U;
    if (!_opened)
    {
        return NIVALO_STORE_UNREADABLE;
    }
    size_t stored = 0U;
    const NivaloStoreRead lookup = readStatus(nvs_get_blob(_handle, key, NULL, &stored));
    if (lookup != NIVALO_STORE_OK)
    {
        return lookup;
    }
    if (stored == 0U || stored > capacity)
    {
        return NIVALO_STORE_CORRUPT;
    }
    size_t read = stored;
    const esp_err_t error = nvs_get_blob(_handle, key, data, &read);
    if (error != ESP_OK || read != stored)
    {
        // The key was found a moment ago: a failed data read is never absence.
        return NIVALO_STORE_UNREADABLE;
    }
    *length = read;
    return NIVALO_STORE_OK;
}

NivaloStoreRead NivaloDeviceStore::loadBoot(uint32_t *boot)
{
    if (!_opened || boot == NULL)
    {
        return NIVALO_STORE_UNREADABLE;
    }
    uint32_t value = 0U;
    const NivaloStoreRead read = readStatus(nvs_get_u32(_handle, BootKey, &value));
    if (read != NIVALO_STORE_OK)
    {
        return read;
    }
    *boot = value;
    return value == 0xFFFFFFFFUL ? NIVALO_STORE_CORRUPT : NIVALO_STORE_OK;
}

bool NivaloDeviceStore::writeBoot(uint32_t boot)
{
    uint32_t check = boot + 1U;
    return _opened && nvs_set_u32(_handle, BootKey, boot) == ESP_OK && commit() &&
           nvs_get_u32(_handle, BootKey, &check) == ESP_OK && check == boot;
}

NivaloStoreRead NivaloDeviceStore::loadJournal(NivaloJournal *journal)
{
    uint8_t blob[NivaloJournalBlobBytes];
    size_t length = 0U;
    const NivaloStoreRead read = readBlob(JournalKey, blob, sizeof(blob), &length);
    if (read != NIVALO_STORE_OK)
    {
        return read;
    }
    return nivaloDecodeJournal(blob, length, journal) ? NIVALO_STORE_OK : NIVALO_STORE_CORRUPT;
}

bool NivaloDeviceStore::writeJournal(const NivaloJournal &journal)
{
    uint8_t blob[NivaloJournalBlobBytes];
    const size_t length = nivaloEncodeJournal(journal, blob, sizeof(blob));
    return length > 0U && writeBlob(JournalKey, blob, length);
}

NivaloStoreRead NivaloDeviceStore::loadPending(NivaloPendingRecord *pending)
{
    uint8_t blob[NivaloPendingBlobBytes];
    size_t length = 0U;
    const NivaloStoreRead read = readBlob(PendingKey, blob, sizeof(blob), &length);
    if (read != NIVALO_STORE_OK)
    {
        return read;
    }
    return nivaloDecodePending(blob, length, pending) ? NIVALO_STORE_OK : NIVALO_STORE_CORRUPT;
}

bool NivaloDeviceStore::writePending(const NivaloPendingRecord &pending)
{
    uint8_t blob[NivaloPendingBlobBytes];
    const size_t length = nivaloEncodePending(pending, blob, sizeof(blob));
    return length > 0U && writeBlob(PendingKey, blob, length);
}

bool NivaloDeviceStore::clearPending()
{
    if (!_opened)
    {
        return false;
    }
    const esp_err_t erased = nvs_erase_key(_handle, PendingKey);
    if ((erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) || !commit())
    {
        return false;
    }
    size_t length = 0U;
    return nvs_get_blob(_handle, PendingKey, NULL, &length) == ESP_ERR_NVS_NOT_FOUND;
}

NivaloStoreRead NivaloDeviceStore::loadFloor(const char *productId, uint32_t *floor)
{
    *floor = 0U;
    uint8_t blob[NivaloFloorBlobBytes];
    size_t length = 0U;
    const NivaloStoreRead read = readBlob(FloorKey, blob, sizeof(blob), &length);
    if (read != NIVALO_STORE_OK)
    {
        return read;
    }
    NivaloFloorRecord record;
    if (!nivaloDecodeFloor(blob, length, &record))
    {
        return NIVALO_STORE_CORRUPT;
    }
    if (productId == NULL || strcmp(record.productId, productId) != 0)
    {
        // The floor of another product (a USB flash into the other environment).
        return NIVALO_STORE_MISSING;
    }
    *floor = record.floor;
    return NIVALO_STORE_OK;
}

bool NivaloDeviceStore::writeFloor(const char *productId, uint32_t floor)
{
    if (!nivaloIsUuid(productId))
    {
        return false;
    }
    NivaloFloorRecord record;
    memcpy(record.productId, productId, sizeof(record.productId));
    record.floor = floor;
    uint8_t blob[NivaloFloorBlobBytes];
    const size_t length = nivaloEncodeFloor(record, blob, sizeof(blob));
    return length > 0U && writeBlob(FloorKey, blob, length);
}

NivaloStoreRead NivaloDeviceStore::loadCatalog(NivaloCatalogState *state)
{
    uint8_t *blob = new (std::nothrow) uint8_t[NivaloCatalogBlobBytes];
    if (blob == NULL)
    {
        return NIVALO_STORE_UNREADABLE;
    }
    size_t length = 0U;
    NivaloStoreRead result = readBlob(CatalogKey, blob, NivaloCatalogBlobBytes, &length);
    if (result == NIVALO_STORE_OK && !nivaloDecodeCatalog(blob, length, state))
    {
        result = NIVALO_STORE_CORRUPT;
    }
    delete[] blob;
    return result;
}

bool NivaloDeviceStore::writeCatalog(const NivaloCatalogState &state)
{
    uint8_t *blob = new (std::nothrow) uint8_t[NivaloCatalogBlobBytes];
    if (blob == NULL)
    {
        return false;
    }
    const size_t length = nivaloEncodeCatalog(state, blob, NivaloCatalogBlobBytes);
    const bool written = length > 0U && writeBlob(CatalogKey, blob, length);
    delete[] blob;
    return written;
}

NivaloIdentityAdmission NivaloDeviceStore::admit(const char *deviceId, bool allowFirstEnrollment)
{
    if (!begin() || !nivaloIsUuid(deviceId))
    {
        return NIVALO_IDENTITY_REFUSED;
    }
    NivaloJournal journal;
    const NivaloStoreRead journalRead = loadJournal(&journal);
    uint32_t boot = 0U;
    const NivaloStoreRead bootRead = loadBoot(&boot);
    if (journalRead == NIVALO_STORE_OK && strcmp(journal.deviceId, deviceId) == 0)
    {
        // Same device ID: its history stays. Without a readable boot counter it must not connect,
        // and reseeding it could repeat presence epochs.
        return bootRead == NIVALO_STORE_OK ? NIVALO_IDENTITY_KEEP : NIVALO_IDENTITY_REFUSED;
    }
    if (allowFirstEnrollment && journalRead == NIVALO_STORE_MISSING && bootRead == NIVALO_STORE_MISSING)
    {
        return NIVALO_IDENTITY_FIRST_ENROLLMENT;
    }
    // Lost or unreadable records, or another device ID's history: manually supplied credentials
    // cannot prove that this device ID never executed anything.
    return NIVALO_IDENTITY_REFUSED;
}

NivaloIdentityAdmission NivaloDeviceStore::admitManualCredentials(const char *deviceId)
{
    return admit(deviceId, false);
}

NivaloIdentityAdmission NivaloDeviceStore::admitDeveloperFixture(const char *deviceId)
{
    return admit(deviceId, true);
}

bool NivaloDeviceStore::initializeIdentity(const char *deviceId)
{
    if (!begin() || !nivaloIsUuid(deviceId))
    {
        return false;
    }
    // A record whose read failed is never written over: it may read back on a later attempt.
    NivaloJournal journal;
    const NivaloStoreRead journalRead = loadJournal(&journal);
    uint32_t boot = 0U;
    const NivaloStoreRead bootRead = loadBoot(&boot);
    if (journalRead == NIVALO_STORE_UNREADABLE || bootRead == NIVALO_STORE_UNREADABLE)
    {
        return false;
    }
    const bool keep = journalRead == NIVALO_STORE_OK && strcmp(journal.deviceId, deviceId) == 0;
    if (!keep && !writeJournal(nivaloFreshJournal(deviceId)))
    {
        return false;
    }
    if (bootRead != NIVALO_STORE_OK && !writeBoot(0U))
    {
        return false;
    }
    return identityReady(deviceId);
}

bool NivaloDeviceStore::identityReady(const char *deviceId)
{
    if (!begin() || !nivaloIsUuid(deviceId))
    {
        return false;
    }
    NivaloJournal journal;
    uint32_t boot = 0U;
    return loadJournal(&journal) == NIVALO_STORE_OK && strcmp(journal.deviceId, deviceId) == 0 &&
           loadBoot(&boot) == NIVALO_STORE_OK;
}
