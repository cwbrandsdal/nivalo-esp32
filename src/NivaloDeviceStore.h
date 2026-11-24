#ifndef NIVALO_DEVICE_STORE_H
#define NIVALO_DEVICE_STORE_H

// Durable protocol state in its own NVS namespace ("nivalo-dev"). Wi-Fi changes, credential
// resets and factoryReset() of the provisioning store leave it alone (specs/cloud-mqtt-v1.1.md
// section 15): boot counter, command journal, OTA pending record and floor, capability catalog.
// Every critical write is read back before it is relied on.

#include <Arduino.h>
#include <Preferences.h>

#include "NivaloCatalog.h"
#include "NivaloJournal.h"
#include "NivaloOtaPolicy.h"

enum NivaloStoreRead
{
    NIVALO_STORE_OK = 0,
    NIVALO_STORE_MISSING,
    NIVALO_STORE_CORRUPT
};

class NivaloDeviceStore
{
public:
    bool begin();
    void end();

    NivaloStoreRead loadBoot(uint32_t *boot);
    bool writeBoot(uint32_t boot);

    NivaloStoreRead loadJournal(NivaloJournal *journal);
    bool writeJournal(const NivaloJournal &journal);

    NivaloStoreRead loadPending(NivaloPendingRecord *pending);
    bool writePending(const NivaloPendingRecord &pending);
    bool clearPending();

    // The committed floor of productId; 0 when none was committed for this product.
    uint32_t loadFloor(const char *productId);
    bool writeFloor(const char *productId, uint32_t floor);

    NivaloStoreRead loadCatalog(NivaloCatalogState *state);
    bool writeCatalog(const NivaloCatalogState &state);

    // Called when provisioning commits an identity. A device ID without a journal gets a fresh
    // one (highWater 0); an existing journal of the same device ID is kept. A corrupt journal is
    // replaced only for a newly claimed identity. A missing boot counter starts at 0.
    bool prepareIdentity(const char *deviceId, bool newlyClaimed);

private:
    bool writeBlob(const char *key, const uint8_t *data, size_t length);
    size_t readBlob(const char *key, uint8_t *data, size_t capacity, bool *present);

    Preferences _preferences;
    bool _opened = false;
};

#endif
