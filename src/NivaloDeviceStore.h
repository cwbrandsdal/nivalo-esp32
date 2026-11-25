#ifndef NIVALO_DEVICE_STORE_H
#define NIVALO_DEVICE_STORE_H

// Durable protocol state in its own NVS namespace ("nivalo-dev"). Wi-Fi changes, credential
// resets and factoryReset() of the provisioning store leave it alone (specs/cloud-mqtt-v1.1.md
// section 15): boot counter, command journal, OTA pending record and floor, capability catalog.
// Every critical write is read back before it is relied on.
//
// The native NVS API is used, not Preferences: Preferences reports every lookup error as an
// absent key, and an unreadable floor or journal must never pass for a missing one. Only
// ESP_ERR_NVS_NOT_FOUND reads as NIVALO_STORE_MISSING; every other error is
// NIVALO_STORE_UNREADABLE.

#include <Arduino.h>
#include <nvs.h>

#include "NivaloCatalog.h"
#include "NivaloJournal.h"
#include "NivaloOtaPolicy.h"

enum NivaloIdentityAdmission
{
    // The journal and the boot counter belong to this device ID: keep them, write nothing.
    NIVALO_IDENTITY_KEEP = 0,
    // No protocol state on this unit at all: the developer fixture may initialize it.
    NIVALO_IDENTITY_FIRST_ENROLLMENT,
    // Missing or unreadable records, or another device ID's state. Nothing may be written; the
    // credentials are refused (claim the device instead).
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
    // product (*floor 0); CORRUPT or UNREADABLE when a record may exist but cannot be read. Then
    // OTA must stay disabled and the record is left as it is for recovery.
    NivaloStoreRead loadFloor(const char *productId, uint32_t *floor);
    bool writeFloor(const char *productId, uint32_t floor);

    // Never write over a catalog that did not read OK or MISSING.
    NivaloStoreRead loadCatalog(NivaloCatalogState *state);
    bool writeCatalog(const NivaloCatalogState &state);

    // --- Identity (section 15) -------------------------------------------------------------------
    // The journal's high-water mark and the boot counter are the replay and presence history of a
    // device ID. Only an authoritative claim (a new device identity) may initialize them, or the
    // local developer fixture (off in every stock profile) on a unit with no protocol state.

    // USB credentials (nivalo.cli.provision.v1) for deviceId: KEEP only when the journal of
    // deviceId and the boot counter read back; anything else, an empty unit included, is REFUSED.
    // A board with empty NVS cannot prove that a supplied, previously used ID never executed.
    NivaloIdentityAdmission admitManualCredentials(const char *deviceId);
    // The compiled local developer fixture: as admitManualCredentials(), and FIRST_ENROLLMENT when
    // neither a journal nor a boot counter exists (both genuinely not found).
    NivaloIdentityAdmission admitDeveloperFixture(const char *deviceId);
    // After a claim of deviceId or an admitted fixture enrollment: a journal of deviceId that reads
    // back is kept, anything else is replaced by a fresh one (highWater 0); a readable boot
    // counter is kept, otherwise it starts at 0. Idempotent, so an interrupted commit repeats it.
    bool initializeIdentity(const char *deviceId);
    // The journal of deviceId and the boot counter both read back.
    bool identityReady(const char *deviceId);

private:
    NivaloIdentityAdmission admit(const char *deviceId, bool allowFirstEnrollment);
    bool commit();
    bool writeBlob(const char *key, const uint8_t *data, size_t length);
    // OK with *length bytes in data, MISSING, UNREADABLE, or CORRUPT when the stored blob is
    // empty or larger than capacity (no record of this format).
    NivaloStoreRead readBlob(const char *key, uint8_t *data, size_t capacity, size_t *length);

    nvs_handle_t _handle = 0;
    bool _opened = false;
};

#endif
