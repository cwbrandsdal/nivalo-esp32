#ifndef NIVALO_EXECUTOR_H
#define NIVALO_EXECUTOR_H

// The device's single executor (specs/cloud-mqtt-v1.1.md sections 9, 10.4 and 11.4). Every
// journal transition (claim, rejection before the effect, terminal result, boot reconciliation)
// happens here, on one task. The table check and the transition it leads to are one call, so
// two deliveries of one sequence never both execute.

#include <stddef.h>
#include <stdint.h>

#include "NivaloClock.h"
#include "NivaloDownlink.h"
#include "NivaloJournal.h"
#include "NivaloOtaPolicy.h"

struct NivaloAck
{
    NivaloOperationClass operationClass = NIVALO_OPERATION_COMMAND;
    char id[37] = {0};
    uint32_t seq = 0U;
    NivaloResult result;
    // The clock was invalid when the result was reached: publish once it is valid again.
    bool deferred = false;
};

enum NivaloFunctionOutcome
{
    NIVALO_FUNCTION_SUCCEEDED = 0,
    // The handler knows the effect did not happen.
    NIVALO_FUNCTION_FAILED = 1,
    // The effect may have happened (timeout, error after an output may have changed).
    NIVALO_FUNCTION_UNKNOWN = 2,
    // Still running; the application reports the outcome later with the invocation's token.
    NIVALO_FUNCTION_PENDING = 3
};

enum NivaloDownloadOutcome
{
    // Exactly sizeBytes bytes with the signed SHA-256 and the manifest's embedded identity, and
    // the image passed validation.
    NIVALO_DOWNLOAD_VERIFIED = 0,
    NIVALO_DOWNLOAD_REJECTED = 1,
    NIVALO_DOWNLOAD_PENDING = 2,
    // Stopped because NivaloExecutor::deadlinePermits() failed: the image could no longer be
    // activated. The executor records expired or clock_unsynced from a fresh reading.
    NIVALO_DOWNLOAD_ABORTED = 3
};

enum NivaloActivationOutcome
{
    NIVALO_ACTIVATION_SELECTED = 0,
    // The image was definitely not selected; the running image stays.
    NIVALO_ACTIVATION_REJECTED = 1,
    // The call failed in a way that may have selected the image.
    NIVALO_ACTIVATION_UNCERTAIN = 2
};

enum NivaloRefreshOutcome
{
    // Every packet of the catalog was published.
    NIVALO_REFRESH_COMPLETE = 0,
    // Nothing was published (catalog unreadable or not storable, or the first packet failed).
    NIVALO_REFRESH_NOT_STARTED = 1,
    // Some packets were published, not all.
    NIVALO_REFRESH_PARTIAL = 2
};

class NivaloExecutor;

class NivaloExecutorPlatform
{
public:
    virtual ~NivaloExecutorPlatform() {}
    // Durable writes, read back before they are relied on. false means not durable.
    virtual bool writeJournal(const NivaloJournal &journal) = 0;
    virtual bool writePending(const NivaloPendingRecord &pending) = 0;
    virtual bool clearPending() = 0;
    virtual bool writeFloor(uint32_t floor) = 0;
    // Reads, with the store's distinction between a missing record, a damaged one and a read
    // that failed. readFloor() is the committed floor of the running product.
    virtual NivaloStoreRead readPending(NivaloPendingRecord *pending) = 0;
    virtual NivaloStoreRead readFloor(uint32_t *floor) = 0;
    // Folds every trusted synchronisation since the last call into the executor
    // (NivaloExecutor::onTrustedSync), then returns a fresh reading.
    virtual NivaloClockReading readClock() = 0;
    virtual void publishAck(const NivaloAck &ack) = 0;
    // Generic commands. prepareFunction() runs before the claim: it parses the operation's
    // original argument bytes once into the object the handler will receive (refusing values
    // that object cannot represent) and runs the handler's own argument check. armPrepared()
    // runs after the claim and before the final clock reading: it accepts completions for token
    // from then on (also from a task the handler starts) and resolves the handler and the
    // argument view, so invokePrepared() is the handler call and nothing else. disarmPrepared()
    // withdraws the token when the final reading rejects the operation. releasePrepared() frees
    // the object; it is called after every prepareFunction().
    virtual bool hasFunction(const char *name) = 0;
    virtual bool prepareFunction(const NivaloOperation &operation) = 0;
    virtual bool armPrepared(uint32_t token) = 0;
    virtual NivaloFunctionOutcome invokePrepared() = 0;
    virtual void disarmPrepared() = 0;
    virtual void releasePrepared() = 0;
    virtual NivaloRefreshOutcome refreshCatalog() = 0;
    // OTA. A download may call NivaloExecutor::deadlinePermits() while it runs.
    virtual NivaloDownloadOutcome downloadImage(const NivaloOperation &operation, uint32_t token) = 0;
    virtual NivaloActivationOutcome activateImage() = 0;
    // SHA-256 (lowercase hex) of the first sizeBytes bytes of the running image's slot.
    virtual bool hashRunningSlot(uint32_t sizeBytes, char sha256Hex[65]) = 0;
    // Restarts the device; does not return on hardware.
    virtual void restart() = 0;
};

class NivaloExecutor
{
public:
    explicit NivaloExecutor(NivaloExecutorPlatform &platform);

    // Boot: adopt the stored journal for deviceId and reconcile a claimed record (section 11.4
    // step 4) before any connection. journal is NULL when it is missing or corrupt; then the
    // executor stays blocked until the device is re-claimed. The pending record and the floor are
    // read through the platform; ota supplies the running image's security version and receives
    // the committed floor (and whether it is readable), which the executor keeps up to date.
    //
    // A claimed OTA whose evidence cannot be read yet (pending record unreadable, or the new
    // image runs but the floor cannot be read) stays claimed with its pending record: reconciling()
    // holds the executor busy until retryReconciliation() can finish it. Success is journaled
    // only after a floor of at least the installed version is durable; a pending record is
    // cleared only once no claim depends on it.
    void begin(const char *deviceId, const NivaloJournal *journal, NivaloOtaDevice *ota,
               const NivaloImageIdentity &runningImage);
    // Reads again what boot reconciliation could not read, and finishes it when it can.
    void retryReconciliation();
    bool reconciling() const { return _reconcile != RECONCILE_NONE; }

    // One identified downlink.
    void deliver(const NivaloOperation &operation);
    // The invocation identified by token, whose handler returned NIVALO_FUNCTION_PENDING,
    // finished. Called on the executor's task only. False (and nothing changes) for a stale,
    // unknown or zero token, for NIVALO_FUNCTION_PENDING, or when the terminal result could not be
    // made durable; busy() ends only once it is.
    bool completeFunction(uint32_t token, NivaloFunctionOutcome outcome);
    // The download identified by token, which returned NIVALO_DOWNLOAD_PENDING, finished.
    bool completeDownload(uint32_t token, NivaloDownloadOutcome outcome);
    // A fresh clock reading against the active operation's deadline (section 9).
    bool deadlinePermits();
    // A trusted SNTP synchronisation (section 9).
    void onTrustedSync(int64_t utcMs, int64_t monotonicUs);
    // After every birth and with every heartbeat: the journaled terminal ACK, from RAM.
    void republishResult();

    bool busy() const { return _busy; }
    // The token of the claimed invocation; 0 when none is active.
    uint32_t activeToken() const { return _busy ? _token : 0U; }
    // No downlink is accepted: missing, corrupt or unwritable journal.
    bool blocked() const { return _blocked; }
    const NivaloJournal &journal() const { return _journal; }
    bool hasPending() const { return _hasPending; }
    const NivaloPendingRecord &pending() const { return _pending; }

private:
    enum Reconcile
    {
        RECONCILE_NONE = 0,
        // The pending record of the claimed OTA could not be read.
        RECONCILE_PENDING,
        // The pending image runs; the floor could not be read, so it cannot be raised yet.
        RECONCILE_FLOOR
    };
    enum ImageVerdict
    {
        IMAGE_UNCHECKED = 0,
        IMAGE_MATCHES,
        IMAGE_DIFFERS
    };

    void readPending();
    void readFloor();
    void reconcile();
    bool runningImageIsPending();
    // A durable floor of at least the pending version (any higher floor kept); false when it is
    // not durable yet (waiting for a readable floor, or blocked on a failed write).
    bool commitFloor();
    void waitFor(Reconcile stage);
    void clearStalePending();
    void execute(const NivaloOperation &operation);
    NivaloAckCode validate(const NivaloOperation &operation);
    // A fresh reading against the deadline; on failure *result is the terminal failure and
    // *deferred tells whether it waits for a valid clock.
    bool recheck(NivaloResult *result, bool *deferred);
    // Journals the terminal result; busy ends and the result is ACKed only once it is durable.
    bool finish(const NivaloResult &result, bool deferred = false);
    // Terminal result, then (only when that is durable) the pending record is cleared.
    void finishOta(const NivaloResult &result, bool deferred = false);
    void afterDownload(NivaloDownloadOutcome outcome);
    void ack(NivaloOperationClass operationClass, const char *id, uint32_t seq, const NivaloResult &result,
             bool deferred = false);
    bool persist(const NivaloJournal &journal);
    uint32_t nextToken();

    NivaloExecutorPlatform &_platform;
    NivaloOtaDevice *_ota = NULL;
    NivaloJournal _journal;
    NivaloPendingRecord _pending;
    // A pending record may be stored (it read, or its read did not report "not found").
    bool _hasPending = false;
    NivaloStoreRead _pendingRead = NIVALO_STORE_MISSING;
    Reconcile _reconcile = RECONCILE_NONE;
    ImageVerdict _imageVerdict = IMAGE_UNCHECKED;
    NivaloImageIdentity _runningImage;
    bool _blocked = true;
    bool _busy = false;
    uint32_t _token = 0U;
    uint32_t _lastToken = 0U;
    NivaloDeadline _deadline;
    // The active operation.
    NivaloOtaManifest _manifest;
};

#endif
