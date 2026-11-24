#ifndef NIVALO_JOURNAL_H
#define NIVALO_JOURNAL_H

// Durable execution records of specs/cloud-mqtt-v1.1.md sections 10.4 and 11.4, and their
// fixed binary encodings (versioned, CRC-protected) for NVS blobs.

#include <stddef.h>
#include <stdint.h>

#include "NivaloOtaPolicy.h"
#include "NivaloWire.h"

enum NivaloAckStatus
{
    NIVALO_ACK_ACCEPTED = 1,
    NIVALO_ACK_RUNNING = 2,
    NIVALO_ACK_SUCCEEDED = 3,
    NIVALO_ACK_FAILED = 4,
    NIVALO_ACK_INDETERMINATE = 5
};

enum NivaloAckCode
{
    NIVALO_CODE_NONE = 0,
    NIVALO_CODE_EXPIRED,
    NIVALO_CODE_CLOCK_UNSYNCED,
    NIVALO_CODE_INVALID_COMMAND,
    NIVALO_CODE_NOT_ALLOWED,
    NIVALO_CODE_ID_CONFLICT,
    NIVALO_CODE_STORAGE_ERROR,
    NIVALO_CODE_OTA_REJECTED,
    NIVALO_CODE_HANDLER_FAILED,
    NIVALO_CODE_EXECUTION_UNKNOWN,
    NIVALO_CODE_REPLAY_SUPPRESSED
};

struct NivaloResult
{
    uint8_t status = 0U;
    uint8_t code = NIVALO_CODE_NONE;
};

NivaloResult nivaloResult(NivaloAckStatus status, NivaloAckCode code = NIVALO_CODE_NONE);
const char *nivaloAckStatusName(uint8_t status);
const char *nivaloAckCodeName(uint8_t code);

enum NivaloJournalPhase
{
    NIVALO_PHASE_CLAIMED = 1,
    NIVALO_PHASE_DONE = 2
};

// High-water mark and the record of that operation, kept as one blob per device ID.
struct NivaloJournal
{
    char deviceId[37] = {0};
    uint32_t highWater = 0U;
    bool hasLast = false;
    uint32_t seq = 0U;
    char commandId[37] = {0};
    uint8_t operationClass = NIVALO_OPERATION_COMMAND;
    uint8_t fingerprint[32] = {0};
    uint8_t phase = NIVALO_PHASE_DONE;
    NivaloResult result;
};

// A fresh journal for a newly claimed device ID: highWater 0, no record.
NivaloJournal nivaloFreshJournal(const char *deviceId);

// The committed OTA floor, bound to the product it was committed for.
struct NivaloFloorRecord
{
    char productId[37] = {0};
    uint32_t floor = 0U;
};

static const size_t NivaloJournalBlobBytes = 128U;
static const size_t NivaloPendingBlobBytes = 128U;
static const size_t NivaloFloorBlobBytes = 64U;

size_t nivaloEncodeJournal(const NivaloJournal &journal, uint8_t *output, size_t capacity);
bool nivaloDecodeJournal(const uint8_t *data, size_t length, NivaloJournal *journal);
size_t nivaloEncodePending(const NivaloPendingRecord &pending, uint8_t *output, size_t capacity);
bool nivaloDecodePending(const uint8_t *data, size_t length, NivaloPendingRecord *pending);
size_t nivaloEncodeFloor(const NivaloFloorRecord &floor, uint8_t *output, size_t capacity);
bool nivaloDecodeFloor(const uint8_t *data, size_t length, NivaloFloorRecord *floor);

uint32_t nivaloCrc32(const uint8_t *data, size_t length);

#endif
