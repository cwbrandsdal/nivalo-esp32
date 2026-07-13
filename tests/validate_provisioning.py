#!/usr/bin/env python3
import json
import base64
from pathlib import Path
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT=Path(__file__).resolve().parents[1]

def transition(c):
    if c["action"]=="button-hold": return "setup-portal-retain-old"
    if c["action"]=="reset" and c.get("pending") and c.get("committed_matches_pending"): return "load-committed-and-clean-pending"
    if c["action"]=="reset" and c.get("pending"): return "load-pending-and-retry-same-attempt"
    if c["claim"]=="http-timeout" and c.get("pending"): return "retry-same-pending-attempt"
    if c["claim"]=="exact-retry" and c.get("pending"): return "idempotent-same-nonsecret-response"
    if c["claim"]=="same-code" and c.get("pending") and c["action"]=="setup-submit": return "reuse-pending-attempt-with-new-wifi"
    if c["claim"]=="mismatched-replay": return "server-reject-no-commit"
    if c["claim"]=="expired" and c["action"]=="retry-limit": return "setup-portal-replace-pending-attempt"
    if not c["stored"] and c["action"]=="boot": return "setup-portal"
    if c["wifi"]=="timeout": return "setup-portal-retain-old"
    if c["action"]=="setup-submit" and c["wifi"]=="connected": return "atomic-wifi-commit-ready"
    if c["claim"]=="valid": return "atomic-commit-ready"
    if c["claim"]=="rejected": return "setup-portal-no-commit"
    return "ready"

def main():
    for c in json.loads((ROOT/"tests/provisioning_state_cases.json").read_text()): assert transition(c)==c["expected"],c["name"]
    source=(ROOT/"src/NivaloProvisioning.cpp").read_text(); header=(ROOT/"src/NivaloProvisioning.h").read_text()
    assert "esp_flash_encryption_enabled" in source and "allowUnencryptedNvsForLocalDevelopment = false" in header
    assert source.index("putString(key, encoded)") < source.index('putString("active", next)')
    assert 'strncmp(_config.claimUrl, "https://", 8)' in source and "setCACert" in source and "setInsecure" not in source
    assert "HTTPC_DISABLE_FOLLOW_REDIRECTS" in source
    assert 'mqtt["port"].as<int>() != 8883 && mqtt["port"].as<int>() != 8884' in source
    assert "mqttPort == 8883U || mqttPort == 8884U" in source
    assert 'doc.containsKey("claimSecret")' in source
    assert "NIVALO-DEVICE-CLAIM-V1\\n" in source and "MBEDTLS_ECP_DP_SECP256R1" in source
    assert source.index("_store.load(_credentials)") < source.index("_store.loadPending(_claimAttempt)")
    assert "pendingIdentityWasCommitted(_credentials, _claimAttempt)" in source
    reconcile_start = source.index("if (hasCredentials && hasPendingAttempt")
    reconcile_end = source.index("else if (hasPendingAttempt)", reconcile_start)
    reconcile_branch = source[reconcile_start:reconcile_end]
    assert "_store.clearPending()" in reconcile_branch
    assert "startWifi(_credentials.wifiSsid, _credentials.wifiPassword)" in reconcile_branch
    assert "exchangeClaim" not in reconcile_branch
    assert source.index("_store.savePending(_claimAttempt)") < source.index("startWifi(ssid, password)")
    assert "reusePendingAttempt = _claimAttempt.valid() && claim == _claimAttempt.claimCode" in source
    reuse_start = source.index("if (reusePendingAttempt)", source.index("void NivaloProvisioning::handlePortalSubmit"))
    reuse_end = source.index("else if (!createPendingAttempt", reuse_start)
    reuse_branch = source[reuse_start:reuse_end]
    assert "_claimAttempt.wifiSsid = ssid" in reuse_branch
    assert "_claimAttempt.wifiPassword = password" in reuse_branch
    assert "createPendingAttempt" not in reuse_branch
    for preserved in ("attemptId", "nonce", "publicKeyPem", "privateKeyPem", "mqttCredential", "credentialSha256", "signatureBase64"):
        assert f"_claimAttempt.{preserved} =" not in reuse_branch
    for field in ('request["attemptId"]', 'request["mqttCredential"]', 'request["proof"]["credentialSha256"]'):
        assert field in source
    assert 'doc["mqtt"]["password"]' not in source
    assert 'mqtt.containsKey("password")' in source and "doc.size() != 2U" in source
    assert 'mqtt.size() == 6U && !mqtt.containsKey("caCertificatePem")' in source
    assert "_claimRetryAt=millis()+min(60000UL" in source
    assert "_claimFailures = 0U; _claimRetryAt = 0U" in source
    claim_success = source.index("if (exchangeClaim() && verifyExistingIdentity() && _store.commit(_pending))")
    assert claim_success < source.index("_store.clearPending()", claim_success)
    assert 'Serial.println(_pendingClaimCode)' not in source and 'Serial.println(password)' not in source
    assert "while (WiFi.status()" not in source
    assert "verifyExistingIdentity() && _store.commit(_pending)" in source and "mqtt.setSocketTimeout(3)" in source
    assert "wifiConnectTimeoutMs = 20000UL" in header and "setupButtonHoldMs = 3000UL" in header
    assert "NIVALO_PROVISIONING_SYNCING_TIME" in header
    assert 'primaryNtpServer = "pool.ntp.org"' in header
    assert "timeSyncTimeoutMs = 15000UL" in header and "timeSyncMaximumAttempts = 3U" in header
    connecting_start = source.index("if (_state == NIVALO_PROVISIONING_CONNECTING_WIFI)")
    syncing_start = source.index("else if (_state == NIVALO_PROVISIONING_SYNCING_TIME)", connecting_start)
    claiming_start = source.index("else if (_state == NIVALO_PROVISIONING_CLAIMING)", syncing_start)
    connecting_branch = source[connecting_start:syncing_start]
    syncing_branch = source[syncing_start:claiming_start]
    assert "startTimeSync(true)" in connecting_branch
    assert "exchangeClaim" not in connecting_branch and "verifyExistingIdentity" not in connecting_branch
    assert "clockPermitsTls()" in syncing_branch and "continueAfterTimeSync()" in syncing_branch
    assert "exchangeClaim" not in syncing_branch and "verifyExistingIdentity" not in syncing_branch
    assert "timeSyncWaitingToRetry" in syncing_branch and "mayRetry" in syncing_branch and "startTimeSync(false)" in syncing_branch
    exchange_start = source.index("bool NivaloProvisioning::exchangeClaim()")
    exchange_http = source.index("HTTPClient http", exchange_start)
    assert source.index("if (!clockPermitsTls())", exchange_start) < exchange_http
    verify_start = source.index("bool NivaloProvisioning::verifyExistingIdentity()")
    verify_socket = source.index("WiFiClientSecure tls", verify_start)
    assert source.index("if (!clockPermitsTls())", verify_start) < verify_socket
    for rel in ("examples/Esp32Only","examples/Esp32Stm32Bridge"):
        main_source=(ROOT/rel/"src/main.cpp").read_text(); cfg=(ROOT/rel/"include/nivalo_config.example.h").read_text()
        assert "NivaloProvisioning provisioning" in main_source and "provisioning.loop()" in main_source
        assert "NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE 0" in cfg
        assert "NIVALO_WIFI_SSID" not in cfg and "NIVALO_IOT_MQTT_PASSWORD" not in cfg
        assert "NIVALO_MQTT_TRANSPORT_TLS" in main_source
    assert not any(token in source for token in (
        'Serial.println(body)', 'Serial.println(response)', 'Serial.println(signature)',
        'Serial.println(_claimAttempt.claimCode)', 'Serial.println(_claimAttempt.mqttCredential)'))
    vector=json.loads((ROOT/"tests/claim_proof_vector.json").read_text())
    import hashlib
    assert hashlib.sha256(vector["mqttCredential"].encode()).hexdigest()==vector["credentialSha256"]
    manifest=f'NIVALO-DEVICE-CLAIM-V1\n{vector["attemptId"]}\n{vector["claimCode"]}\n{vector["hardwareId"]}\n{vector["nonce"]}\n{vector["credentialSha256"]}\n'.encode()
    assert manifest.decode()==vector["manifestUtf8"]
    public=serialization.load_pem_public_key(vector["publicKeyPem"].encode())
    public.verify(base64.b64decode(vector["signatureBase64"]),manifest,ec.ECDSA(hashes.SHA256()))
    for tampered in (manifest.replace(b"ABCD2345",b"ABCD2346"),manifest.replace(b"aabb",b"aabc"),manifest[:-2]+b"0\n"):
        try: public.verify(base64.b64decode(vector["signatureBase64"]),tampered,ec.ECDSA(hashes.SHA256()))
        except InvalidSignature: pass
        else: raise AssertionError("tampered claim proof verified")
    print("validated provisioning state, encrypted atomic store, HTTPS proof, replay-safe fixtures, and opt-in developer credentials")
if __name__=="__main__": main()
