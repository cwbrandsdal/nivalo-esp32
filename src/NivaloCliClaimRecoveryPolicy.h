#ifndef NIVALO_CLI_CLAIM_RECOVERY_POLICY_H
#define NIVALO_CLI_CLAIM_RECOVERY_POLICY_H

enum NivaloCliClaimRecoveryAction
{
    NIVALO_CLI_CLAIM_REJECT_REPLAY,
    NIVALO_CLI_CLAIM_PRESERVE_STATE,
    NIVALO_CLI_CLAIM_CONNECT_WIFI,
    NIVALO_CLI_CLAIM_SYNC_TIME,
    NIVALO_CLI_CLAIM_READY
};

namespace NivaloCliClaimRecoveryPolicy
{
inline NivaloCliClaimRecoveryAction resumeAction(
    bool readyState,
    bool recoveryRequired,
    bool wifiConnected,
    bool clockValid)
{
    if (!readyState && !recoveryRequired) return NIVALO_CLI_CLAIM_REJECT_REPLAY;
    if (readyState) return NIVALO_CLI_CLAIM_PRESERVE_STATE;
    if (!wifiConnected) return NIVALO_CLI_CLAIM_CONNECT_WIFI;
    if (!clockValid) return NIVALO_CLI_CLAIM_SYNC_TIME;
    return NIVALO_CLI_CLAIM_READY;
}
} // namespace NivaloCliClaimRecoveryPolicy

#endif
