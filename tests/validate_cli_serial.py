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
    assert "root.size() != 4U" in source
    assert "wifi.size() != 2U || mqtt.size() != 8U" in source
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

    print("validated bounded CLI NDJSON, strict credential policy, encrypted atomic commit, and non-secret responses")


if __name__ == "__main__":
    main()
