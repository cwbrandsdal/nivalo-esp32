# Phase 1 ESP32 hardware drill

Use this checklist only during an approved attended test window. The prepared
standalone tester is `D:\GitHub\nivalo-projects\ESP32\ESP32-WROOM-TESTER`.
Its PlatformIO environment targets `COM6` and a `115200` baud monitor. Building
does not authorize upload, reset, serial access, credential rotation, or a live
command/OTA operation.

Never paste Wi-Fi, MQTT, claim, signing-private-key, or token values into the
evidence record. Record stable identifiers, timestamps, hashes, and sanitized
outcomes instead.

## Preconditions

- [ ] Record the tester project and `nivalo-esp32` commit/worktree identity.
- [ ] Run `pio run` and retain its success result and firmware SHA-256.
- [ ] Confirm the intended board is physically on `COM6`; do not guess from a
      previously cached port.
- [ ] Confirm the monitor will use `115200` baud.
- [ ] Confirm MQTT is explicitly TLS on port `8883`; public `1883` is not used.
- [ ] Confirm the test device belongs to the intended organization and no
      production customer device shares its identity.
- [ ] Confirm rollback firmware/configuration and the operator responsible for
      restoring them.

## TLS positive and negative cases

### Valid trust and hostname

- [ ] Flash only after separate approval, then open `COM6` at `115200`.
- [ ] Confirm time synchronization completes before MQTT connection.
- [ ] Confirm TLS/8883 connects using the expected hostname and trusted CA.
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
- [ ] Restore the approved hostname and prove TLS/8883 reconnects successfully.

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
| Device ID / hardware revision | |
| Tester and library revision | |
| Firmware SHA-256 | |
| COM port / baud | `COM6` / `115200` |
| Broker hostname / port | hostname only / `8883` |
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

