#include "NivaloCliTransaction.h"

#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char *message)
{
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

struct FakeOperations
{
    int failAfter = 0;
    bool failureInjected = false;
    int step = 0;
    bool staged = false;
    bool active = false;
    bool pendingClaim = true;
    bool verified = false;
    bool stateReady = false;

    bool resultAfterMutation()
    {
        ++step;
        if (!failureInjected && step == failAfter)
        {
            failureInjected = true;
            return false;
        }
        return true;
    }
    bool stage() { staged = true; return resultAfterMutation(); }
    bool commitActive() { active = true; return resultAfterMutation(); }
    bool supersedePendingClaim() { pendingClaim = false; return resultAfterMutation(); }
    bool verifyActive() { verified = active; return resultAfterMutation(); }
    bool prepareState() { stateReady = verified; return resultAfterMutation(); }
    bool clearStage() { staged = false; return resultAfterMutation(); }
};
} // namespace

int main()
{
    for (int boundary = 1; boundary <= 6; ++boundary)
    {
        FakeOperations operations;
        operations.failAfter = boundary;
        require(!NivaloCliTransaction::start(operations), "injected boundary unexpectedly acknowledged");
        require(NivaloCliTransaction::resume(operations), "durable transaction did not recover");
        require(operations.active, "recovery lost active identity");
        require(!operations.pendingClaim, "recovery retained superseded claim");
        require(operations.verified, "recovery did not verify active identity");
        require(operations.stateReady, "recovery cleared the stage before the protocol state was ready");
        require(!operations.staged, "recovery did not clear durable stage");
    }

    FakeOperations success;
    require(NivaloCliTransaction::start(success), "successful transaction rejected");
    require(success.step == 6, "transaction step count drifted");
    require(success.active && success.verified && success.stateReady && !success.pendingClaim && !success.staged,
            "successful transaction final state invalid");
    return 0;
}
