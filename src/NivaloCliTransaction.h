#ifndef NIVALO_CLI_TRANSACTION_H
#define NIVALO_CLI_TRANSACTION_H

namespace NivaloCliTransaction
{
template <typename Operations>
bool resume(Operations &operations)
{
    // The stage is the recovery marker: it is cleared only after the credentials and the protocol
    // state are both durable and read back.
    return operations.commitActive() &&
           operations.supersedePendingClaim() &&
           operations.verifyActive() &&
           operations.prepareState() &&
           operations.clearStage();
}

template <typename Operations>
bool start(Operations &operations)
{
    return operations.stage() && resume(operations);
}
} // namespace NivaloCliTransaction

#endif
