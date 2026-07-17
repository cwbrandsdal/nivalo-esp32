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
