import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    source = (ROOT / "src/NivaloProvisioning.cpp").read_text()
    header = (ROOT / "src/NivaloProvisioning.h").read_text()
    policy = (ROOT / "src/NivaloCliProvisioningPolicy.cpp").read_text()
    policy_header = (ROOT / "src/NivaloCliProvisioningPolicy.h").read_text()
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
    assert 'expectedHardwareId != hardwareId()' in source
    assert "isHardwareId" in policy and "isClaimCode" in policy
    assert source.index("_store.savePending(_claimAttempt)", source.index("bool NivaloProvisioning::handleCliClaim")) < source.index(
        "startWifi(wifiSsid, wifiPassword)", source.index("bool NivaloProvisioning::handleCliClaim")
    )
    assert 'response["schema"] = "nivalo.cli.claim.v1"' in source
    assert 'response["hardwareId"] = hardwareId()' in source
    assert 'response["deviceId"] = _credentials.deviceId' in source
    assert 'response["claim"]' not in source
    active_serializer = source[source.index("static String serializeCredentials") : source.index("static const char CliStageTombstone")]
    assert 'doc["claimCodeSha256"]' in active_serializer
    assert 'doc["claimCode"]' not in active_serializer
    claim_handler = source[source.index("bool NivaloProvisioning::handleCliClaim") : source.index("bool NivaloProvisioning::finishCliProvisioning")]
    assert '_credentials.claimCodeSha256 != claimCodeSha256' in claim_handler
    assert claim_handler.index('_credentials.claimCodeSha256 != claimCodeSha256') < claim_handler.index('sendCliClaimResponse(true)')
    assert claim_handler.index('sendCliClaimResponse(true)') < claim_handler.index('createPendingAttempt')
    assert claim_handler.index('sendCliClaimResponse(true)') < claim_handler.index('startWifi(wifiSsid, wifiPassword)')
    assert 'sha256Hex(_claimAttempt.claimCode, _pending.claimCodeSha256)' in source

    print("validated bounded CLI NDJSON, serial-bound device claim, encrypted atomic commit, and non-secret responses")


if __name__ == "__main__":
    main()
