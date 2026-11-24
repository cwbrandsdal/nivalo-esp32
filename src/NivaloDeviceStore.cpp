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
} // namespace

bool NivaloDeviceStore::begin()
{
    if (!_opened)
    {
        _opened = _preferences.begin(Namespace, false);
    }
    return _opened;
}

void NivaloDeviceStore::end()
{
    if (_opened)
    {
        _preferences.end();
        _opened = false;
    }
}

bool NivaloDeviceStore::writeBlob(const char *key, const uint8_t *data, size_t length)
{
    if (!_opened || data == NULL || length == 0U)
    {
        return false;
    }
    if (_preferences.putBytes(key, data, length) != length)
    {
        return false;
    }
    uint8_t *check = new (std::nothrow) uint8_t[length];
    if (check == NULL)
    {
        return false;
    }
    const bool same = _preferences.getBytesLength(key) == length && _preferences.getBytes(key, check, length) == length &&
                      memcmp(check, data, length) == 0;
    delete[] check;
    return same;
}

size_t NivaloDeviceStore::readBlob(const char *key, uint8_t *data, size_t capacity, bool *present)
{
    *present = _opened && _preferences.isKey(key);
    if (!*present)
    {
        return 0U;
    }
    const size_t length = _preferences.getBytesLength(key);
    if (length == 0U || length > capacity)
    {
        return 0U;
    }
    return _preferences.getBytes(key, data, length);
}

NivaloStoreRead NivaloDeviceStore::loadBoot(uint32_t *boot)
{
    if (!_opened || boot == NULL)
    {
        return NIVALO_STORE_CORRUPT;
    }
    if (!_preferences.isKey(BootKey))
    {
        return NIVALO_STORE_MISSING;
    }
    *boot = _preferences.getULong(BootKey, 0xFFFFFFFFUL);
    return *boot == 0xFFFFFFFFUL ? NIVALO_STORE_CORRUPT : NIVALO_STORE_OK;
}

bool NivaloDeviceStore::writeBoot(uint32_t boot)
{
    return _opened && _preferences.putULong(BootKey, boot) == sizeof(uint32_t) &&
           _preferences.getULong(BootKey, boot + 1U) == boot;
}

NivaloStoreRead NivaloDeviceStore::loadJournal(NivaloJournal *journal)
{
    uint8_t blob[NivaloJournalBlobBytes];
    bool present = false;
    const size_t length = readBlob(JournalKey, blob, sizeof(blob), &present);
    if (!present)
    {
        return _opened ? NIVALO_STORE_MISSING : NIVALO_STORE_CORRUPT;
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
    bool present = false;
    const size_t length = readBlob(PendingKey, blob, sizeof(blob), &present);
    if (!present)
    {
        return NIVALO_STORE_MISSING;
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
    if (_preferences.isKey(PendingKey))
    {
        _preferences.remove(PendingKey);
    }
    return !_preferences.isKey(PendingKey);
}

NivaloStoreRead NivaloDeviceStore::loadFloor(const char *productId, uint32_t *floor)
{
    *floor = 0U;
    uint8_t blob[NivaloFloorBlobBytes];
    bool present = false;
    const size_t length = readBlob(FloorKey, blob, sizeof(blob), &present);
    if (!present)
    {
        return _opened ? NIVALO_STORE_MISSING : NIVALO_STORE_CORRUPT;
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
        return NIVALO_STORE_CORRUPT;
    }
    bool present = false;
    const size_t length = readBlob(CatalogKey, blob, NivaloCatalogBlobBytes, &present);
    NivaloStoreRead result = NIVALO_STORE_MISSING;
    if (present)
    {
        result = nivaloDecodeCatalog(blob, length, state) ? NIVALO_STORE_OK : NIVALO_STORE_CORRUPT;
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

NivaloIdentityAdmission NivaloDeviceStore::admitIdentity(const char *deviceId, bool hasActiveIdentity)
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
    if (!hasActiveIdentity && journalRead == NIVALO_STORE_MISSING && bootRead == NIVALO_STORE_MISSING)
    {
        return NIVALO_IDENTITY_FIRST_ENROLLMENT;
    }
    // Lost records of an identity that exists, or another device ID's history: manually supplied
    // credentials cannot prove that this device ID never executed anything on this unit.
    return NIVALO_IDENTITY_REFUSED;
}

bool NivaloDeviceStore::initializeIdentity(const char *deviceId)
{
    if (!begin() || !nivaloIsUuid(deviceId))
    {
        return false;
    }
    NivaloJournal journal;
    const bool keep = loadJournal(&journal) == NIVALO_STORE_OK && strcmp(journal.deviceId, deviceId) == 0;
    if (!keep && !writeJournal(nivaloFreshJournal(deviceId)))
    {
        return false;
    }
    uint32_t boot = 0U;
    if (loadBoot(&boot) != NIVALO_STORE_OK && !writeBoot(0U))
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
