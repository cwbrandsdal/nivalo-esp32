#include "NivaloExecutor.h"

#include <string.h>

namespace
{
NivaloResult functionResult(NivaloFunctionOutcome outcome)
{
    switch (outcome)
    {
    case NIVALO_FUNCTION_SUCCEEDED:
        return nivaloResult(NIVALO_ACK_SUCCEEDED);
    case NIVALO_FUNCTION_FAILED:
        return nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_HANDLER_FAILED);
    default:
        // Anything the handler cannot vouch for may have had an effect.
        return nivaloResult(NIVALO_ACK_INDETERMINATE, NIVALO_CODE_EXECUTION_UNKNOWN);
    }
}
} // namespace

NivaloExecutor::NivaloExecutor(NivaloExecutorPlatform &platform) : _platform(platform) {}

bool NivaloExecutor::persist(const NivaloJournal &journal)
{
    if (!_platform.writeJournal(journal))
    {
        // An unwritable journal cannot guarantee at-most-once: accept nothing until re-claimed.
        _blocked = true;
        return false;
    }
    _journal = journal;
    return true;
}

void NivaloExecutor::begin(const char *deviceId, const NivaloJournal *journal, const NivaloPendingRecord *pending,
                           NivaloOtaDevice *ota, const NivaloImageIdentity &runningImage)
{
    _ota = ota;
    _busy = false;
    _deadline.stop();
    _hasPending = pending != NULL;
    if (pending != NULL)
    {
        _pending = *pending;
    }
    if (journal == NULL || deviceId == NULL || strcmp(journal->deviceId, deviceId) != 0)
    {
        _journal = NivaloJournal();
        _blocked = true;
        return;
    }
    _journal = *journal;
    _blocked = false;

    if (_journal.hasLast && _journal.phase == NIVALO_PHASE_CLAIMED)
    {
        NivaloResult result = nivaloResult(NIVALO_ACK_INDETERMINATE, NIVALO_CODE_EXECUTION_UNKNOWN);
        if (_journal.operationClass == NIVALO_OPERATION_OTA && _hasPending && _pending.commandSeq == _journal.seq)
        {
            char slotSha256[65] = {0};
            const bool hashed = _platform.hashRunningSlot(_pending.sizeBytes, slotSha256);
            if (nivaloImageMatchesPending(runningImage, hashed ? slotSha256 : NULL, _pending))
            {
                // Floor, then result, then clear: each write is idempotent, so a reset in between
                // repeats this step at the next boot.
                const uint32_t floor = _ota != NULL && _ota->committedFloor > _pending.securityVersion
                                           ? _ota->committedFloor
                                           : _pending.securityVersion;
                if (!_platform.writeFloor(floor))
                {
                    _blocked = true;
                    return;
                }
                if (_ota != NULL)
                {
                    _ota->committedFloor = floor;
                }
                result = nivaloResult(NIVALO_ACK_SUCCEEDED);
            }
        }
        NivaloJournal done = _journal;
        done.phase = NIVALO_PHASE_DONE;
        done.result = result;
        if (!persist(done))
        {
            return;
        }
    }
    if (_hasPending)
    {
        // A pending record that does not belong to a claimed OTA record is just cleared. A failed
        // clear is retried at the next boot; the record no longer matches any claim.
        if (_platform.clearPending())
        {
            _hasPending = false;
        }
    }
}

void NivaloExecutor::ack(NivaloOperationClass operationClass, const char *id, uint32_t seq,
                         const NivaloResult &result, bool deferred)
{
    NivaloAck message;
    message.operationClass = operationClass;
    memcpy(message.id, id, sizeof(message.id));
    message.id[36] = '\0';
    message.seq = seq;
    message.result = result;
    message.deferred = deferred;
    _platform.publishAck(message);
}

void NivaloExecutor::republishResult()
{
    if (!_blocked && _journal.hasLast && _journal.phase == NIVALO_PHASE_DONE)
    {
        ack((NivaloOperationClass)_journal.operationClass, _journal.commandId, _journal.seq, _journal.result);
    }
}

void NivaloExecutor::onTrustedSync(int64_t utcMs, int64_t monotonicUs)
{
    _deadline.observe(utcMs, monotonicUs);
}

void NivaloExecutor::deliver(const NivaloOperation &operation)
{
    if (_blocked || operation.seq == 0U || !nivaloSeqMatchesId(operation.seq, operation.id))
    {
        return;
    }
    if (operation.seq < _journal.highWater)
    {
        ack(operation.operationClass, operation.id, operation.seq,
            nivaloResult(NIVALO_ACK_INDETERMINATE, NIVALO_CODE_REPLAY_SUPPRESSED));
        return;
    }
    if (operation.seq == _journal.highWater)
    {
        const bool same = _journal.hasLast && strcmp(_journal.commandId, operation.id) == 0 &&
                          _journal.operationClass == (uint8_t)operation.operationClass &&
                          memcmp(_journal.fingerprint, operation.fingerprint, sizeof(operation.fingerprint)) == 0;
        if (!same)
        {
            ack(operation.operationClass, operation.id, operation.seq,
                nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_ID_CONFLICT));
            return;
        }
        ack(operation.operationClass, operation.id, operation.seq,
            _busy ? nivaloResult(NIVALO_ACK_RUNNING) : _journal.result);
        return;
    }
    if (_busy)
    {
        // Not consumed; the server redelivers once this operation has ended.
        return;
    }
    execute(operation);
}

NivaloAckCode NivaloExecutor::validate(const NivaloOperation &operation)
{
    if (!operation.valid)
    {
        return NIVALO_CODE_INVALID_COMMAND;
    }
    if (operation.operationClass == NIVALO_OPERATION_OTA)
    {
        return _ota != NULL && nivaloOtaPrecheck(*_ota, operation) == NIVALO_OTA_PRECHECK_OK ? NIVALO_CODE_NONE
                                                                                          : NIVALO_CODE_OTA_REJECTED;
    }
    if (strcmp(operation.command, "requestDefinitions") == 0)
    {
        return NIVALO_CODE_NONE;
    }
    if (!_platform.hasFunction(operation.command))
    {
        return NIVALO_CODE_NOT_ALLOWED;
    }
    return _platform.validArguments(operation.command, operation.arguments) ? NIVALO_CODE_NONE
                                                                           : NIVALO_CODE_INVALID_COMMAND;
}

void NivaloExecutor::execute(const NivaloOperation &operation)
{
    const NivaloClockReading claimReading = _platform.readClock();
    NivaloAckCode failure = validate(operation);
    bool deferred = false;
    if (failure == NIVALO_CODE_NONE)
    {
        if (!claimReading.valid)
        {
            failure = NIVALO_CODE_CLOCK_UNSYNCED;
            deferred = true;
        }
        else if (operation.issuedAtMs > claimReading.utcMs + NivaloV11::IssuedAheadMs)
        {
            failure = NIVALO_CODE_CLOCK_UNSYNCED;
        }
        else if (claimReading.utcMs >= operation.expiresAtMs)
        {
            failure = NIVALO_CODE_EXPIRED;
        }
    }

    NivaloJournal record = _journal;
    record.highWater = operation.seq;
    record.hasLast = true;
    record.seq = operation.seq;
    memcpy(record.commandId, operation.id, sizeof(record.commandId));
    record.operationClass = (uint8_t)operation.operationClass;
    memcpy(record.fingerprint, operation.fingerprint, sizeof(record.fingerprint));
    record.result = NivaloResult();

    if (failure != NIVALO_CODE_NONE)
    {
        // A rejection before the effect consumes the sequence.
        record.phase = NIVALO_PHASE_DONE;
        record.result = nivaloResult(NIVALO_ACK_FAILED, failure);
        if (persist(record))
        {
            ack(operation.operationClass, operation.id, operation.seq, record.result, deferred);
        }
        return;
    }

    record.phase = NIVALO_PHASE_CLAIMED;
    if (!persist(record))
    {
        return;
    }
    _busy = true;
    _manifest = operation.manifest;
    _deadline.start(operation.expiresAtMs, claimReading);
    ack(operation.operationClass, operation.id, operation.seq, nivaloResult(NIVALO_ACK_ACCEPTED));

    if (operation.operationClass == NIVALO_OPERATION_OTA)
    {
        const NivaloDownloadOutcome outcome = _platform.downloadImage(operation);
        if (outcome != NIVALO_DOWNLOAD_PENDING)
        {
            afterDownload(outcome);
        }
        return;
    }

    if (!recheck())
    {
        return;
    }
    if (strcmp(operation.command, "requestDefinitions") == 0)
    {
        _platform.refreshCatalog();
        finish(nivaloResult(NIVALO_ACK_SUCCEEDED));
        return;
    }
    const NivaloFunctionOutcome outcome = _platform.invokeFunction(operation.command, operation.arguments);
    if (outcome != NIVALO_FUNCTION_PENDING)
    {
        finish(functionResult(outcome));
    }
}

bool NivaloExecutor::recheck()
{
    // A fresh reading immediately before the effect; one taken earlier never counts.
    const NivaloClockReading reading = _platform.readClock();
    bool clockInvalid = false;
    if (_deadline.permits(reading, &clockInvalid))
    {
        return true;
    }
    finish(nivaloResult(NIVALO_ACK_FAILED, clockInvalid ? NIVALO_CODE_CLOCK_UNSYNCED : NIVALO_CODE_EXPIRED),
           clockInvalid);
    return false;
}

void NivaloExecutor::finish(const NivaloResult &result, bool deferred)
{
    NivaloJournal done = _journal;
    done.phase = NIVALO_PHASE_DONE;
    done.result = result;
    _busy = false;
    _deadline.stop();
    if (persist(done))
    {
        ack((NivaloOperationClass)done.operationClass, done.commandId, done.seq, done.result, deferred);
    }
}

void NivaloExecutor::completeFunction(NivaloFunctionOutcome outcome)
{
    if (!_busy || _journal.operationClass != NIVALO_OPERATION_COMMAND || outcome == NIVALO_FUNCTION_PENDING)
    {
        return;
    }
    finish(functionResult(outcome));
}

void NivaloExecutor::completeDownload(NivaloDownloadOutcome outcome)
{
    if (!_busy || _journal.operationClass != NIVALO_OPERATION_OTA || outcome == NIVALO_DOWNLOAD_PENDING)
    {
        return;
    }
    afterDownload(outcome);
}

void NivaloExecutor::afterDownload(NivaloDownloadOutcome outcome)
{
    if (outcome != NIVALO_DOWNLOAD_VERIFIED)
    {
        finish(nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_OTA_REJECTED));
        return;
    }
    NivaloPendingRecord pending;
    pending.commandSeq = _journal.seq;
    memcpy(pending.productId, _manifest.productId, sizeof(pending.productId));
    pending.securityVersion = _manifest.securityVersion;
    pending.sizeBytes = _manifest.sizeBytes;
    memcpy(pending.sha256, _manifest.sha256, sizeof(pending.sha256));
    if (!_platform.writePending(pending))
    {
        finish(nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_STORAGE_ERROR));
        _hasPending = !_platform.clearPending();
        return;
    }
    _pending = pending;
    _hasPending = true;

    // The reading that gates activation is taken after the pending record is durable.
    if (!recheck())
    {
        _hasPending = !_platform.clearPending();
        return;
    }
    // Exactly one activation call per operation.
    const NivaloActivationOutcome activation = _platform.activateImage();
    if (activation == NIVALO_ACTIVATION_REJECTED)
    {
        finish(nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_OTA_REJECTED));
        _hasPending = !_platform.clearPending();
        return;
    }
    // Selected, or possibly selected: keep the claim and the pending record; boot reconciliation
    // decides what is running.
    _platform.restart();
}
