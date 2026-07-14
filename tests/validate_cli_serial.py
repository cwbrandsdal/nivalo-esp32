import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    source = (ROOT / "src/NivaloProvisioning.cpp").read_text()
    header = (ROOT / "src/NivaloProvisioning.h").read_text()
    policy = (ROOT / "src/NivaloCliProvisioningPolicy.cpp").read_text()
    policy_header = (ROOT / "src/NivaloCliProvisioningPolicy.h").read_text()
    recovery_policy = (ROOT / "src/NivaloCliClaimRecoveryPolicy.h").read_text()
    frame = (ROOT / "src/NivaloCliFrame.cpp").read_text()
    transaction = (ROOT / "src/NivaloCliTransaction.h").read_text()

    assert 'schema == "nivalo.cli.identify.v1"' in source
    assert 'schema == "nivalo.cli.provision.v1"' in source
    assert 'schema == "nivalo.cli.claim.v1"' in source
    assert "root.size() != 4U" in source
    assert "wifi.size() != 2U || mqtt.size() != 7U" in source
    assert 'mqtt["useTls"].as<bool>() != true' in source
    assert "isTlsPort" in source and "port == 8883U || port == 8884U" in policy
    assert "finishCliProvisioning(replacement, true)" in source
    assert "root.size() != 5U" in source
    assert "wifi.size() != 2U || claim.size() != 1U" in source
    assert "expectedHardwareId != hardwareId()" in source
    assert "isClaimCode" in source and "length != 8U" in policy
    assert "_store.savePending(_claimAttempt)" in source
    claim_dispatch = source[source.index('else if (schema == "nivalo.cli.claim.v1")') :]
    assert claim_dispatch.index("handleCliClaim(root, requestId)") < claim_dispatch.index(
        "sendCliClaimResponse(requestId, false)"
    )
    claim_success = source[source.index("if (exchangeClaim() && verifyExistingIdentity()") :]
    assert claim_success.index("_store.commit(_pending)") < claim_success.index(
        "sendCliClaimResponse(_cliClaimRequestId, true)"
    )
    assert claim_success.index("_store.clearPending()") < claim_success.index(
        "sendCliClaimResponse(_cliClaimRequestId, true)"
    )
    assert 'response["hardwareId"] = currentHardwareId' in source
    assert 'response["deviceId"] = _credentials.deviceId' in source
    assert 'doc["claimCodeSha256"]' in source
    active_serializer = source[source.index("static String serializeCredentials") : source.index("static const char CliStageTombstone")]
    assert 'doc["claimCode"]' not in active_serializer
    assert "sha256Text(_claimAttempt.claimCode, _pending.claimCodeSha256)" in source
    assert "constantTimeEqual(_credentials.claimCodeSha256, claimReceipt)" in source
    assert "_credentials.wifiSsid == wifiSsid" in source
    assert "constantTimeEqual(_credentials.wifiPassword, wifiPassword)" in source
    wifi_change = source[source.index("if (_changingWifiOnly)") : source.index("else if (_pendingClaimCode")]
    assert '_pending.claimCodeSha256 = ""' in wifi_change
    assert "_pending.claimCodeSha256 = _credentials.claimCodeSha256" not in wifi_change
    assert "NivaloCliClaimRecoveryPolicy::resumeAction" in source
    assert "if (readyState || ordinaryStartup) return NIVALO_CLI_CLAIM_PRESERVE_STATE" in recovery_policy
    assert "if (!recoveryRequired) return NIVALO_CLI_CLAIM_REJECT_REPLAY" in recovery_policy
    assert "NivaloCliClaimRecoveryPolicy::isOrdinaryStartup" in source
    ordinary_startup = source[source.index("const bool ordinaryStartup") : source.index("const bool recoveryRequired")]
    assert "NIVALO_PROVISIONING_CONNECTING_WIFI" in ordinary_startup
    assert "NIVALO_PROVISIONING_SYNCING_TIME" in ordinary_startup
    assert "_claimAttempt.valid()" in ordinary_startup
    assert "_changingWifiOnly" in ordinary_startup
    assert "recoveryAction == NIVALO_CLI_CLAIM_REJECT_REPLAY" in source
    replay_start = source.index("const bool readyState")
    replay_end = source.index("_claimAttempt = NivaloPendingClaimAttempt()", replay_start)
    replay_guard = source[replay_start:replay_end]
    assert replay_guard.index("NIVALO_CLI_CLAIM_REJECT_REPLAY") < replay_guard.index("_store.clearPending()")
    assert "_changingWifiOnly = false" not in replay_guard
    provision_handler = source[source.index("bool NivaloProvisioning::handleCliProvision") :]
    assert "_cliClaimRequestId.length() > 0U" in provision_handler.split("JsonObject wifi", 1)[0]
    dispatch = source[source.index("const bool ok = handleCliProvision") :]
    assert dispatch.index("handleCliProvision(root, requestId)") < dispatch.index(
        'sendCliResponse("nivalo.cli.provision.v1", requestId, ok)'
    )
    assert '_preferences.getString("active", "") != next' in source
    assert "serializeCredentials(committed) == encoded" in source
    assert transaction.index("operations.stage()") < transaction.index("resume(operations)")
    assert transaction.index("operations.commitActive()") < transaction.index("operations.supersedePendingClaim()")
    assert transaction.index("operations.supersedePendingClaim()") < transaction.index("operations.verifyActive()")
    assert transaction.index("operations.verifyActive()") < transaction.index("operations.clearStage()")
    recovery = source[source.index("NivaloRuntimeCredentials cliStaged") :]
    assert recovery.index("_store.loadCliStaged(cliStaged)") < recovery.index("_store.load(_credentials)")
    assert '_preferences.getString("pending", "") == PendingClaimTombstone' in source
    assert '_preferences.getString("cliStage", "") == CliStageTombstone' in source

    assert "MaximumLineLength = 16384U" in policy_header
    assert "FrameTimeoutMs = UINT32_C(5000)" in policy_header
    assert "_length >= _maximumLength" in frame
    assert "processed++ < 256U" in source
    assert "now - _startedAt < _timeoutMs" in frame
    assert "volatile char *wipe" in frame

    assert "allowUnencryptedNvsForLocalDevelopment = false" in header
    assert "storeAvailable && _config.localDeveloperFixture == NULL" in source
    assert "esp_flash_encryption_enabled" in source
    assert 'setError("CLI serial buffer allocation failed")' in source

    forbidden = (
        "Serial.println(line)",
        "Serial.print(line)",
        'response["wifi"]',
        'response["mqtt"]',
        'response["password"]',
        'response["ssid"]',
        'response["claim"]',
        'response["claimCode"]',
        'response["claimCodeSha256"]',
    )
    for value in forbidden:
        assert value not in source
    assert "serializeJson(response, *_cliSerial)" in source

    fixture = json.loads((ROOT / "tests/cli_provision_request_v1.json").read_text())
    assert set(fixture) == {"schema", "requestId", "wifi", "mqtt"}
    assert set(fixture["wifi"]) == {"ssid", "password"}
    assert set(fixture["mqtt"]) == {
        "deviceId", "host", "port", "useTls", "clientId", "username", "password"
    }
    assert len(fixture["mqtt"]) == 7

    claim_fixture = json.loads((ROOT / "tests/cli_claim_request_v1.json").read_text())
    assert set(claim_fixture) == {"schema", "requestId", "expectedHardwareId", "wifi", "claim"}
    assert set(claim_fixture["wifi"]) == {"ssid", "password"}
    assert set(claim_fixture["claim"]) == {"code"}

    print("validated bounded CLI identify, claim, and provision with strict policy, durable commit, and non-secret responses")


if __name__ == "__main__":
    main()
