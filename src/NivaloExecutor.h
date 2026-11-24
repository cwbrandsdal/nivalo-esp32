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
    // Still running; the application reports the outcome later.
    NIVALO_FUNCTION_PENDING = 3
};

enum NivaloDownloadOutcome
{
    // Exactly sizeBytes bytes with the signed SHA-256 and the manifest's embedded identity, and
    // the image passed validation.
    NIVALO_DOWNLOAD_VERIFIED = 0,
    NIVALO_DOWNLOAD_REJECTED = 1,
    NIVALO_DOWNLOAD_PENDING = 2
};

enum NivaloActivationOutcome
{
    NIVALO_ACTIVATION_SELECTED = 0,
    // The image was definitely not selected; the running image stays.
    NIVALO_ACTIVATION_REJECTED = 1,
    // The call failed in a way that may have selected the image.
    NIVALO_ACTIVATION_UNCERTAIN = 2
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
    // Folds every trusted synchronisation since the last call into the executor
    // (NivaloExecutor::onTrustedSync), then returns a fresh reading.
    virtual NivaloClockReading readClock() = 0;
    virtual void publishAck(const NivaloAck &ack) = 0;
    // Generic commands.
    virtual bool hasFunction(const char *name) = 0;
    virtual bool validArguments(const char *name, const char *argumentsJson) = 0;
    virtual NivaloFunctionOutcome invokeFunction(const char *name, const char *argumentsJson) = 0;
    virtual void refreshCatalog() = 0;
    // OTA.
    virtual NivaloDownloadOutcome downloadImage(const NivaloOperation &operation) = 0;
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
    // executor stays blocked until the device is re-claimed. ota supplies the running image's
    // security version and committed floor; the executor updates the floor.
    void begin(const char *deviceId, const NivaloJournal *journal, const NivaloPendingRecord *pending,
               NivaloOtaDevice *ota, const NivaloImageIdentity &runningImage);

    // One identified downlink.
    void deliver(const NivaloOperation &operation);
    // A handler that returned NIVALO_FUNCTION_PENDING finished.
    void completeFunction(NivaloFunctionOutcome outcome);
    // A download that returned NIVALO_DOWNLOAD_PENDING finished.
    void completeDownload(NivaloDownloadOutcome outcome);
    // A trusted SNTP synchronisation (section 9).
    void onTrustedSync(int64_t utcMs, int64_t monotonicUs);
    // After every birth and with every heartbeat: the journaled terminal ACK, from RAM.
    void republishResult();

    bool busy() const { return _busy; }
    // No downlink is accepted: missing, corrupt or unwritable journal.
    bool blocked() const { return _blocked; }
    const NivaloJournal &journal() const { return _journal; }
    bool hasPending() const { return _hasPending; }
    const NivaloPendingRecord &pending() const { return _pending; }

private:
    void execute(const NivaloOperation &operation);
    NivaloAckCode validate(const NivaloOperation &operation);
    bool recheck();
    void finish(const NivaloResult &result, bool deferred = false);
    void afterDownload(NivaloDownloadOutcome outcome);
    void ack(NivaloOperationClass operationClass, const char *id, uint32_t seq, const NivaloResult &result,
             bool deferred = false);
    bool persist(const NivaloJournal &journal);

    NivaloExecutorPlatform &_platform;
    NivaloOtaDevice *_ota = NULL;
    NivaloJournal _journal;
    NivaloPendingRecord _pending;
    bool _hasPending = false;
    bool _blocked = true;
    bool _busy = false;
    NivaloDeadline _deadline;
    // The active operation.
    NivaloOtaManifest _manifest;
};

#endif
