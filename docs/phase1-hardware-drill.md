# Phase 1 ESP32 hardware drill

Use this checklist only during an approved attended test window. The prepared
standalone tester is `D:\GitHub\nivalo-projects\ESP32\ESP32-WROOM-TESTER`.
Its PlatformIO environment targets `COM6` and a `115200` baud monitor. Building
does not authorize upload, reset, serial access, credential rotation, or a live
command/OTA operation.

For the factory-claim and serial-provisioning cases, build the versioned
`examples/Esp32Only` application with the
`nodemcu-32s-phase1-bench` environment. It pins the connected original ESP32
bench board and `COM6` but remains a direct-flash application output, not an F11
browser artifact. Use an ignored `include/nivalo_config.h` to select isolated
staging and the explicit evaluation-only unencrypted-NVS policy; keep developer
fixtures disabled and never compile Wi-Fi, MQTT, or claim values into it.

Never paste Wi-Fi, MQTT, claim, signing-private-key, or token values into the
evidence record. Record stable identifiers, timestamps, hashes, and sanitized
outcomes instead.

Select exactly one approved environment before building the tester. Do not mix
an API, console, device identity, broker, CA, or firmware-signing key across
environments.

| Environment | MQTT hostname | TLS port |
|---|---|---|
| Production | `mqtt.nivalo.io` | `8883` |
| Isolated staging | `mqtt-staging.nivalo.io` | `8884` |

The table records the repository-approved endpoints, not proof that either is
live. Before an attended drill, independently verify DNS/network publication
and the certificate presented by the selected endpoint. The leaf certificate
must cover the exact hostname. Record the leaf SAN, issuer/intermediate chain,
root subject and SHA-256 fingerprint, and expiry without recording any private
key material. The firmware default trust store accepts Let's Encrypt ISRG Root
X1/X2. If the selected endpoint uses another approved public root, configure
that root explicitly through `NIVALO_DEV_MQTT_CA_CERTIFICATE` (pre-provisioned
tester) or the claim response's `mqtt.caCertificatePem` (runtime provisioning),
and record the CA source. Never use `setInsecure` or pin a short-lived leaf as
the long-lived trust anchor.

## Preconditions

- [ ] Record the tester project and `nivalo-esp32` commit/worktree identity.
- [ ] Run `pio run` and retain its success result and firmware SHA-256.
- [ ] Confirm the intended board is physically on `COM6`; do not guess from a
      previously cached port.
- [ ] Confirm the monitor will use `115200` baud.
- [ ] Record the selected environment and confirm the API, console, device
      identity, broker, and firmware-signing public key all belong to it.
- [ ] Confirm MQTT is explicitly TLS on the selected approved port (`8883` for
      production or `8884` for isolated staging); plaintext `1883`/`1884` is
      not used by the device.
- [ ] Confirm the configured hostname exactly matches the certificate SAN and
      record the verified CA chain/root fingerprint and CA source.
- [ ] Confirm the test device belongs to the intended organization and no
      production customer device shares its identity.
- [ ] Confirm rollback firmware/configuration and the operator responsible for
      restoring them.

## Attended staging claim and standalone LED acceptance

The tools have deliberately separate authority. The authenticated staging
console issues the short-lived claim code and later invokes the function. The
CLI only identifies the USB board and carries the operator-entered code and
Wi-Fi values over the selected serial port. The ESP32 itself performs the HTTPS
claim, creates its proof key and MQTT credential, validates MQTT TLS, and
commits the identity. Do not substitute a host-side call to the public exchange
endpoint.

The CLI can complete the positive serial claim and an exact-code recovery
retry. It cannot create a claim code, invoke `setLed`, deliberately forge a
hardware mismatch, factory-reset the board, or distinguish a distributed
rate-limit rejection from another generic claim rejection. Use the console and
the bounded evidence sources in the negative-case table below for those steps.

1. In a private PowerShell window, build or install the reviewed `nivalo` CLI.
   Do not enable terminal transcription. Keep the CLI source revision and the
   firmware revision in the evidence record.
2. Confirm that the already-flashed application was built with the staging
   HTTPS claim URL, `NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE=0`, and only the
   explicit evaluation-board unencrypted-NVS exception. Do not rebuild or
   upload merely to perform this check.
3. Confirm the intended board is currently `COM6`. Close PlatformIO monitors
   and every other process that may hold the port.
4. Sign in to `https://iot-staging.nivalo.io` as an administrator. Open **Add
   device**, choose **Create claim code**, and keep the code in that private
   browser/terminal session. Do not copy it to chat, a ticket, shell history,
   or the evidence file.
5. Run the CLI without secret-bearing arguments or environment variables:

   ```powershell
   nivalo device claim --port COM6 --baud 115200 --timeout-seconds 300
   ```

   Enter the claim code (or canonical QR URI), Wi-Fi SSID, and Wi-Fi password
   only at the CLI's non-echoing prompts. The CLI identifies the device before
   accepting these values and binds the request to that exact hardware
   identity.
6. Confirm the CLI succeeds, the exact console claim changes to `claimed`, and
   the same device becomes online. Record only timestamps, repository
   revisions, the non-secret device row ID, and PASS/FAIL. Do not retain the
   hardware identity, claim/QR value, Wi-Fi values, MQTT identity, proof, token,
   or raw serial request/response.
7. Confirm MQTT uses `mqtt-staging.nivalo.io:8884`, TLS is certificate- and
   hostname-valid, and the console receives availability plus runtime
   telemetry. A TCP connection alone is not MQTT TLS acceptance.
8. Review and approve the device-reported capability set if it is pending.
   Confirm `setLed` and `ledState` are present. In **Functions**, call `setLed`
   with the JSON string `"on"`, then `"off"`. For each call, retain the command
   ID, prove `accepted` precedes terminal `succeeded`, and confirm the physical
   LED and the next published `ledState` agree. On active-low boards, record the
   observed electrical polarity rather than rewriting the logical result.
9. Send one unsupported benign command through the reviewed command surface
   and confirm terminal `failed`. Do not use firmware, reset, or destructive
   commands for this check.

### Claim recovery and negative-case coverage

Run destructive cases only while the board has no committed identity. Run the
successful lost-response recovery last: after an identity is committed, a
different code is intentionally rejected until an explicit factory reset or
audited transfer. Never erase or reflash merely to make a failed case pass.

| Case | Attended action | Required proof and recovery |
|---|---|---|
| Serial hardware mismatch | With the unclaimed board attended on `COM6`, run `python scripts/phase1_cli_mismatch_probe.py --port COM6`. The probe first identifies the board, substitutes a different syntactically valid expected hardware identity in memory, and sends fixed non-secret placeholders. The normal CLI cannot generate this request. | The generic failed acknowledgement is only a provisional observation because every claim rejection is intentionally indistinguishable. Classify this case PASS only after the same immutable image, with no erase/reflash or configuration change, completes the positive claim below. Retain the probe's single `OBSERVED` line and revisions only; never either identity. |
| Expired code | Issue a code, allow the console countdown to reach zero, then submit it through the normal masked CLI prompts while the board is unclaimed. | CLI/device fail generically; console remains `expired`; no device row or active identity is created. Replace the pending attempt with a newly issued code only after recording the result. |
| Reset before commit | Issue a fresh code and start the normal serial claim. Reset the ESP32 only after the request has been accepted and its pending attempt is durable, but before the console reports `claimed`. Let the first CLI invocation time out, then rerun with the exact same code and Wi-Fi values. | The second run resumes the same durable attempt and creates at most one device. Confirm one claim row, one device, and no credential rotation. Do not disclose the attempt, proof, or credential. |
| Lost terminal response around commit | Start a fresh claim and, after the console reports that exact claim as `claimed` but before the CLI receives its terminal acknowledgement, disconnect USB or terminate only the CLI. Console `claimed` proves API consumption but occurs before MQTT verification and the local commit, so it is not by itself a post-commit discriminator. Reconnect the same board and rerun with the exact same code and Wi-Fi values before expiry. | In correlation-bound audit evidence, a new `device-claim-retry-returned` event means the first interruption was pre-commit and the server exact-retry path succeeded. CLI success with no new retry event, the same device, and no rotation is evidence of the encrypted local receipt path, which makes no API exchange. Record the observed branch; if the CLI had already acknowledged or the audit discriminator is unavailable, record the local-receipt case NOT EXECUTED. |
| Exact retry after expiry | After a successful lost-response retry, allow the original code's bounded expiry to pass before attempting any server exchange replay. | The server-side integration acceptance must prove generic rejection after expiry and no rotation. The active device may continue normal MQTT operation; do not factory-reset it solely for this API property. |
| Distributed rate limit | Do not infer this from a generic CLI rejection. The current repositories have deterministic single- and multi-instance tests but no reviewed live staging claim-rate harness or correlation seam. Implement and review that harness before attempting this case against both API lanes. | Until that prerequisite exists, record live rate-limit acceptance OPEN and cite only the green repository tests. A future harness must prove the shared Redis hardware/code/source-IP budget, denial, window recovery, and unchanged database state while retaining only aggregate counters/timestamps and lane identities. |
| Wi-Fi change | After the positive claim, hold the configured setup button for three seconds and submit new Wi-Fi with a blank claim code through the captive setup flow. | The device keeps the same identity and MQTT credential, reconnects with TLS, and returns online without a firmware rebuild. Restore the approved bench Wi-Fi before closeout. |

Repository evidence for the server-only expiry, mismatch, lost-response,
concurrency, lockout, and distributed-rate properties is in
`Nivalo.IoT.Api.Tests` (`DeviceClaimServiceTests`,
`DeviceClaimEndpointTests`, and `DistributedDeviceClaimRateLimiterTests`).
Firmware transaction and exact-retry coverage is in `tests/host` plus
`tests/validate_cli_serial.py`, `tests/validate_provisioning.py`, and the pinned
cross-repository claim fixture. Automated coverage is required but does not
replace the positive physical claim, TLS, LED, and ACK observations above.

## TLS positive and negative cases

### Valid trust and hostname

- [ ] Flash only after separate approval, then open `COM6` at `115200`.
- [ ] Confirm time synchronization completes before MQTT connection.
- [ ] Confirm TLS connects on the selected approved port using the exact
      environment hostname and recorded trusted CA chain.
- [ ] Confirm the console reports the device online and receives one telemetry
      sample. Record timestamps and the device identifier, not credentials.

### Invalid CA

- [ ] Build a one-test variant with a deliberately unrelated public test CA.
- [ ] Confirm TLS fails closed and MQTT never reports connected/online.
- [ ] Capture the sanitized certificate-validation failure and restore the
      approved CA configuration before the next case.

### Invalid hostname

- [ ] Build a one-test variant whose configured hostname does not match the
      broker certificate while retaining certificate validation.
- [ ] Confirm hostname validation fails and no MQTT session is established.
- [ ] Restore the selected approved hostname/port/CA and prove TLS reconnects
      successfully.

## Provisioning persistence and exact retry

Use the runtime-provisioning standalone example for this case; the compatibility
tester sketch intentionally exercises pre-existing credentials.

- [ ] Start from an approved factory-reset test device with no working identity.
- [ ] Confirm the captive setup flow appears and accepts Wi-Fi plus a short-lived
      claim code without logging either value.
- [ ] Interrupt power/network after the persisted claim attempt and before the
      response is promoted.
- [ ] Reboot and confirm the exact same nonce/proof/credential attempt is retried,
      not regenerated, and the API returns the same identity idempotently.
- [ ] Confirm the promoted identity survives another reboot and comes online.
- [ ] Change Wi-Fi through setup mode and confirm no firmware rebuild is needed.

## Standalone command and ACK lifecycle

- [ ] Confirm definitions show `setLed` and `ledState` for the standalone tester.
- [ ] Invoke `setLed` from the console with a benign value.
- [ ] Record the command ID and prove `accepted` precedes terminal `succeeded`.
- [ ] Confirm GPIO state and the published `ledState` value agree.
- [ ] Send one unsupported command and confirm it terminates as `failed` rather
      than being silently dropped.

## Signed and tampered OTA

- [ ] Provision only the approved public P-256 verification key on the device;
      keep the private signing key in the signing service.
- [ ] Upload a uniquely versioned test artifact through the API and record its
      artifact ID, SHA-256, size, key ID, and signature algorithm.
- [ ] Send the valid signed artifact and confirm signature verification occurs
      before `Update.begin`; confirm reboot and reported firmware version.
- [ ] Send an otherwise identical artifact with one byte changed after signing.
- [ ] Confirm the tampered artifact is rejected before flash begins and the prior
      firmware remains bootable and online.
- [ ] Also confirm an unsigned/unknown-key artifact is rejected fail-closed.

## Evidence record

| Field | Value |
|---|---|
| Drill date/time (UTC) | |
| Operators / approver | |
| Environment (`production` or `isolated staging`) | |
| Device ID / hardware revision | |
| Tester and library revision | |
| Firmware SHA-256 | |
| COM port / baud | `COM6` / `115200` |
| Broker hostname | `mqtt.nivalo.io` or `mqtt-staging.nivalo.io` |
| Broker TLS port | `8883` or `8884`, matching environment |
| Leaf certificate SAN / expiry | |
| Certificate issuer/intermediate chain | subjects and SHA-256 fingerprints |
| Trusted root subject / SHA-256 fingerprint | |
| CA source | firmware default ISRG X1/X2, tester override, or claim response |
| Valid TLS result and timestamp | |
| Invalid-CA result and timestamp | |
| Invalid-hostname result and timestamp | |
| Provisioning retry/persistence result | |
| Command ID and ACK sequence | |
| Valid artifact ID/hash/result | |
| Tampered artifact ID/hash/result | |
| Sanitized log/screenshot locations | |
| Rollback performed / result | |
| Exceptions and follow-up owner | |

Mark the drill complete only when every executed case has sanitized evidence and
the approved configuration is restored.
