#ifndef NIVALO_CLI_TRANSACTION_H
#define NIVALO_CLI_TRANSACTION_H

namespace NivaloCliTransaction
{
template <typename Operations>
bool resume(Operations &operations)
{
    return operations.commitActive() &&
           operations.supersedePendingClaim() &&
           operations.verifyActive() &&
           operations.clearStage();
}

template <typename Operations>
bool start(Operations &operations)
{
    return operations.stage() && resume(operations);
}
} // namespace NivaloCliTransaction

#endif
