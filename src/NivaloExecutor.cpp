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

NivaloResult refreshResult(NivaloRefreshOutcome outcome)
{
    switch (outcome)
    {
    case NIVALO_REFRESH_COMPLETE:
        return nivaloResult(NIVALO_ACK_SUCCEEDED);
    case NIVALO_REFRESH_NOT_STARTED:
        return nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_HANDLER_FAILED);
    default:
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

uint32_t NivaloExecutor::nextToken()
{
    // Unique within a boot; never 0, so a zero-initialised token never matches.
    _lastToken++;
    if (_lastToken == 0U)
    {
        _lastToken = 1U;
    }
    return _lastToken;
}

void NivaloExecutor::begin(const char *deviceId, const NivaloJournal *journal, NivaloOtaDevice *ota,
                           const NivaloImageIdentity &runningImage)
{
    _ota = ota;
    _runningImage = runningImage;
    _busy = false;
    _token = 0U;
    _deadline.stop();
    _reconcile = RECONCILE_NONE;
    _imageVerdict = IMAGE_UNCHECKED;
    readFloor();
    readPending();
    if (journal == NULL || deviceId == NULL || strcmp(journal->deviceId, deviceId) != 0)
    {
        // Whatever pending record exists stays as it is.
        _journal = NivaloJournal();
        _blocked = true;
        return;
    }
    _journal = *journal;
    _blocked = false;
    reconcile();
}

void NivaloExecutor::readPending()
{
    NivaloPendingRecord pending;
    _pendingRead = _platform.readPending(&pending);
    _hasPending = _pendingRead != NIVALO_STORE_MISSING;
    if (_pendingRead == NIVALO_STORE_OK)
    {
        _pending = pending;
    }
}

void NivaloExecutor::readFloor()
{
    if (_ota == NULL)
    {
        return;
    }
    uint32_t floor = 0U;
    const NivaloStoreRead read = _platform.readFloor(&floor);
    // Damaged or unreadable: no security version can be judged against it, so OTA stays off and
    // the record is left for recovery.
    _ota->floorReadable = read == NIVALO_STORE_OK || read == NIVALO_STORE_MISSING;
    _ota->committedFloor = read == NIVALO_STORE_OK ? floor : 0U;
}

void NivaloExecutor::retryReconciliation()
{
    if (_blocked || _reconcile == RECONCILE_NONE)
    {
        return;
    }
    if (_reconcile == RECONCILE_PENDING)
    {
        readPending();
    }
    else
    {
        readFloor();
    }
    reconcile();
}

void NivaloExecutor::waitFor(Reconcile stage)
{
    // The claim and its pending record stay as they are. Busy: the same operation is answered as
    // running, higher sequences are not consumed, nothing new starts.
    _reconcile = stage;
    _busy = true;
}

bool NivaloExecutor::runningImageIsPending()
{
    if (_imageVerdict == IMAGE_UNCHECKED)
    {
        char slotSha256[65] = {0};
        const bool hashed = _platform.hashRunningSlot(_pending.sizeBytes, slotSha256);
        _imageVerdict =
            nivaloImageMatchesPending(_runningImage, hashed ? slotSha256 : NULL, _pending) ? IMAGE_MATCHES : IMAGE_DIFFERS;
    }
    return _imageVerdict == IMAGE_MATCHES;
}

bool NivaloExecutor::commitFloor()
{
    if (_ota != NULL && !_ota->floorReadable)
    {
        // Success is never journaled over an unknown floor: once the pending record is gone, a
        // later slot fallback would find the old floor and accept a version below this one.
        waitFor(RECONCILE_FLOOR);
        return false;
    }
    const uint32_t floor =
        _ota != NULL && _ota->committedFloor > _pending.securityVersion ? _ota->committedFloor : _pending.securityVersion;
    // writeFloor() reads the record back: the floor is durable when it returns true.
    if (!_platform.writeFloor(floor))
    {
        _blocked = true;
        return false;
    }
    if (_ota != NULL)
    {
        _ota->committedFloor = floor;
    }
    return true;
}

void NivaloExecutor::reconcile()
{
    _reconcile = RECONCILE_NONE;
    _busy = false;
    if (_journal.hasLast && _journal.phase == NIVALO_PHASE_CLAIMED)
    {
        NivaloResult result = nivaloResult(NIVALO_ACK_INDETERMINATE, NIVALO_CODE_EXECUTION_UNKNOWN);
        if (_journal.operationClass == NIVALO_OPERATION_OTA)
        {
            if (_pendingRead == NIVALO_STORE_UNREADABLE)
            {
                // Possibly the record of the image now running: decide once it can be read.
                waitFor(RECONCILE_PENDING);
                return;
            }
            if (_pendingRead == NIVALO_STORE_OK && _pending.commandSeq == _journal.seq && runningImageIsPending())
            {
                // Floor, then result, then clear: each write is idempotent, so a reset in between
                // repeats this step at the next boot.
                if (!commitFloor())
                {
                    return;
                }
                result = nivaloResult(NIVALO_ACK_SUCCEEDED);
            }
        }
        NivaloJournal done = _journal;
        done.phase = NIVALO_PHASE_DONE;
        done.result = result;
        if (!persist(done))
        {
            // Blocked. The pending record stays: it is the evidence the next boot reconciles.
            return;
        }
        // Published by republishResult() after the next birth and with every heartbeat.
    }
    clearStalePending();
}

void NivaloExecutor::clearStalePending()
{
    // No claim is open (or its terminal result is durable), so no pending record is evidence of
    // anything: a stale, damaged or unreadable one is cleared. A failed clear is retried at the
    // next boot.
    if (_hasPending && _platform.clearPending())
    {
        _hasPending = false;
        _pendingRead = NIVALO_STORE_MISSING;
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
    return _platform.prepareFunction(operation) ? NIVALO_CODE_NONE : NIVALO_CODE_INVALID_COMMAND;
}

void NivaloExecutor::execute(const NivaloOperation &operation)
{
    const bool generic = operation.operationClass == NIVALO_OPERATION_COMMAND &&
                         strcmp(operation.command, "requestDefinitions") != 0;
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
        if (generic)
        {
            _platform.releasePrepared();
        }
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
        if (generic)
        {
            _platform.releasePrepared();
        }
        return;
    }
    _busy = true;
    _token = nextToken();
    _manifest = operation.manifest;
    _deadline.start(operation.expiresAtMs, claimReading);
    ack(operation.operationClass, operation.id, operation.seq, nivaloResult(NIVALO_ACK_ACCEPTED));

    if (operation.operationClass == NIVALO_OPERATION_OTA)
    {
        const NivaloDownloadOutcome outcome = _platform.downloadImage(operation, _token);
        if (outcome != NIVALO_DOWNLOAD_PENDING)
        {
            afterDownload(outcome);
        }
        return;
    }

    NivaloResult rejected;
    bool rejectedDeferred = false;
    if (!generic)
    {
        // requestDefinitions: the refresh is the effect.
        if (!recheck(&rejected, &rejectedDeferred))
        {
            (void)finish(rejected, rejectedDeferred);
            return;
        }
        (void)finish(refreshResult(_platform.refreshCatalog()));
        return;
    }
    // The handler and its arguments were prepared before the claim. Arming (the completion
    // mailbox for this token, the handler and its argument view) also precedes the fresh reading
    // below, so the handler call is the next step after it.
    if (!_platform.armPrepared(_token))
    {
        // The claim is durable already; nothing ran, so this is a known zero-effect failure.
        _platform.releasePrepared();
        (void)finish(functionResult(NIVALO_FUNCTION_FAILED));
        return;
    }
    if (!recheck(&rejected, &rejectedDeferred))
    {
        _platform.disarmPrepared();
        _platform.releasePrepared();
        (void)finish(rejected, rejectedDeferred);
        return;
    }
    const NivaloFunctionOutcome outcome = _platform.invokePrepared();
    _platform.releasePrepared();
    if (outcome != NIVALO_FUNCTION_PENDING)
    {
        (void)finish(functionResult(outcome));
    }
}

bool NivaloExecutor::recheck(NivaloResult *result, bool *deferred)
{
    // A fresh reading immediately before the effect; one taken earlier never counts.
    const NivaloClockReading reading = _platform.readClock();
    bool clockInvalid = false;
    if (_deadline.permits(reading, &clockInvalid))
    {
        return true;
    }
    *result = nivaloResult(NIVALO_ACK_FAILED, clockInvalid ? NIVALO_CODE_CLOCK_UNSYNCED : NIVALO_CODE_EXPIRED);
    *deferred = clockInvalid;
    return false;
}

bool NivaloExecutor::deadlinePermits()
{
    if (!_busy || !_deadline.active())
    {
        return false;
    }
    bool clockInvalid = false;
    return _deadline.permits(_platform.readClock(), &clockInvalid);
}

bool NivaloExecutor::finish(const NivaloResult &result, bool deferred)
{
    NivaloJournal done = _journal;
    done.phase = NIVALO_PHASE_DONE;
    done.result = result;
    if (!persist(done))
    {
        // Still claimed in flash: stay busy (and blocked), so nothing else can start and a reboot
        // reconciles the claim.
        return false;
    }
    _busy = false;
    _token = 0U;
    _deadline.stop();
    ack((NivaloOperationClass)done.operationClass, done.commandId, done.seq, done.result, deferred);
    return true;
}

void NivaloExecutor::finishOta(const NivaloResult &result, bool deferred)
{
    // Section 11.4 step 3: the result is durable before the pending record is cleared. When it is
    // not, the pending record stays as the evidence boot reconciliation needs.
    if (finish(result, deferred))
    {
        _hasPending = !_platform.clearPending();
    }
}

bool NivaloExecutor::completeFunction(uint32_t token, NivaloFunctionOutcome outcome)
{
    if (_blocked || !_busy || token == 0U || token != _token ||
        _journal.operationClass != NIVALO_OPERATION_COMMAND || outcome == NIVALO_FUNCTION_PENDING)
    {
        return false;
    }
    return finish(functionResult(outcome));
}

bool NivaloExecutor::completeDownload(uint32_t token, NivaloDownloadOutcome outcome)
{
    if (_blocked || !_busy || token == 0U || token != _token || _journal.operationClass != NIVALO_OPERATION_OTA ||
        outcome == NIVALO_DOWNLOAD_PENDING)
    {
        return false;
    }
    afterDownload(outcome);
    return true;
}

void NivaloExecutor::afterDownload(NivaloDownloadOutcome outcome)
{
    NivaloResult rejected;
    bool rejectedDeferred = false;
    if (outcome == NIVALO_DOWNLOAD_ABORTED)
    {
        // The transfer stopped because the image could no longer be activated; record why from a
        // fresh reading.
        if (recheck(&rejected, &rejectedDeferred))
        {
            rejected = nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_OTA_REJECTED);
            rejectedDeferred = false;
        }
        (void)finish(rejected, rejectedDeferred);
        return;
    }
    if (outcome != NIVALO_DOWNLOAD_VERIFIED)
    {
        (void)finish(nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_OTA_REJECTED));
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
        // Possibly partly written: cleared only once the failure is durable.
        finishOta(nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_STORAGE_ERROR));
        return;
    }
    _pending = pending;
    _hasPending = true;

    // The reading that gates activation is taken after the pending record is durable.
    if (!recheck(&rejected, &rejectedDeferred))
    {
        finishOta(rejected, rejectedDeferred);
        return;
    }
    // Exactly one activation call per operation.
    const NivaloActivationOutcome activation = _platform.activateImage();
    if (activation == NIVALO_ACTIVATION_REJECTED)
    {
        finishOta(nivaloResult(NIVALO_ACK_FAILED, NIVALO_CODE_OTA_REJECTED));
        return;
    }
    // Selected, or possibly selected: keep the claim and the pending record; boot reconciliation
    // decides what is running.
    _platform.restart();
}
