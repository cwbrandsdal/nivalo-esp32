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

enum NivaloIdentityAdmission
{
    // The journal and the boot counter belong to this device ID: keep them, write nothing.
    NIVALO_IDENTITY_KEEP = 0,
    // No protocol state and no active identity on this unit: it may be initialized.
    NIVALO_IDENTITY_FIRST_ENROLLMENT,
    // Missing or unreadable records for an identity that exists, or another device ID's state.
    // Nothing may be written; the credentials are refused (claim the device instead).
    NIVALO_IDENTITY_REFUSED
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

    // The committed floor of productId: OK with *floor; MISSING when none was committed for this
    // product (*floor 0); CORRUPT when a record exists but cannot be read. Then OTA must stay
    // disabled and the record is left as it is for recovery.
    NivaloStoreRead loadFloor(const char *productId, uint32_t *floor);
    bool writeFloor(const char *productId, uint32_t floor);

    // CORRUPT also covers a read that could not run (allocation): never write over it.
    NivaloStoreRead loadCatalog(NivaloCatalogState *state);
    bool writeCatalog(const NivaloCatalogState &state);

    // --- Identity (section 15) -------------------------------------------------------------------
    // The journal's high-water mark and the boot counter are the replay and presence history of a
    // device ID. Only an authoritative claim (a new device identity) or a first enrollment may
    // initialize them; every other credential change keeps them or leaves the device blocked.

    // Manually supplied credentials (USB provisioning, a compiled fixture) for deviceId.
    // hasActiveIdentity: credentials of some device ID are already active on this unit.
    NivaloIdentityAdmission admitIdentity(const char *deviceId, bool hasActiveIdentity);
    // After a claim of deviceId or an admitted first enrollment: a journal of deviceId that reads
    // back is kept, anything else is replaced by a fresh one (highWater 0); a readable boot
    // counter is kept, otherwise it starts at 0. Idempotent, so an interrupted commit repeats it.
    bool initializeIdentity(const char *deviceId);
    // The journal of deviceId and the boot counter both read back.
    bool identityReady(const char *deviceId);

private:
    bool writeBlob(const char *key, const uint8_t *data, size_t length);
    size_t readBlob(const char *key, uint8_t *data, size_t capacity, bool *present);

    Preferences _preferences;
    bool _opened = false;
};

#endif
