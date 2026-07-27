# Nivalo ESP32

ESP32 Arduino/PlatformIO library for Nivalo devices.

The attended Phase-1 device acceptance procedure and evidence template are in
[`docs/phase1-hardware-drill.md`](docs/phase1-hardware-drill.md).

The explicit original-ESP32 browser-provisioning artifact build and its
independent partition fingerprint procedure are documented in
[`docs/browser-flash-artifact.md`](docs/browser-flash-artifact.md).

`NivaloDevice` is the primary API for both standalone ESP32 devices and ESP32 devices that bridge to a secondary MCU. Secondary MCU support is enabled at build time with `NIVALO_HAS_SECONDARY_MCU=1`.

## Layout

```text
src/
  NivaloDevice.h
  NivaloDevice.cpp
  NivaloConnection.*
  NivaloReconnectPolicy.*
  NivaloProtocol.*
  NivaloOta.*
  NivaloStatusPixel.*
  NivaloDeviceLifecycle.cpp
  NivaloDeviceCommand.cpp
  NivaloDeviceOta.cpp
  NivaloDeviceLink.cpp
  NivaloDeviceSdk.cpp
  NivaloDeviceMessaging.cpp
  NivaloDeviceReports.cpp
  NivaloDeviceSerial.cpp
  NivaloLinkManager.h
  NivaloLinkSpiTransport.h
  NivaloLinkSpiTransport.cpp
examples/
  Esp32Only/
  Esp32Stm32Bridge/
```

## PlatformIO Dependency

During local development, examples reference this library with:

```ini
lib_deps =
  symlink://../..
```

From GitHub, consumers can use:

```ini
lib_deps =
  https://github.com/cwbrandsdal/nivalo-esp32.git
```

## Configuration

Copy the example config in an example project:

```powershell
Copy-Item .\include\nivalo_config.example.h .\include\nivalo_config.h
```

`include/nivalo_config.h` is ignored by git because it contains device credentials.

New sketches should construct `NivaloDeviceConfig` and call `device.begin(config)`.
It combines MQTT, pin-map, optional secondary UART, watchdog, NTP, and buffering
settings. The positional `beginMqtt(...)` overload remains as a deprecated source
compatibility shim.

## Runtime provisioning and claiming

`NivaloProvisioning` removes compiled production Wi-Fi and MQTT credentials.
On first boot it starts a `Nivalo-Setup-xxxxxx` SoftAP, wildcard DNS captive
portal, and non-blocking web server. The user submits Wi-Fi plus an eight-character
one-use claim code. Wi-Fi is tried for a bounded 20 seconds. Once connected,
provisioning starts SNTP and waits for a valid UTC clock using bounded attempts
with backoff; claim HTTPS and MQTT identity validation fail closed until then.
Before its first HTTPS request, the device generates an ECDSA P-256 proof key
and a 32-byte MQTT credential and persists the entire pending attempt in encrypted Preferences.
The request uses certificate-validating HTTPS and redirects are disabled. Claim codes,
passwords, response bodies, and signatures are never printed.

The response contains only a non-secret device/MQTT identity; the MQTT password
never leaves the device. A lost response or reset reloads and retries the exact
same attempt, allowing the server to return the same identity idempotently.
Submitting the same still-pending code again reuses that attempt and its key
material while allowing new Wi-Fi input; a different code creates a fresh
attempt.
After the returned MQTT TLS identity connects successfully, credentials and the
device proof private key are promoted through an A/B Preferences slot: the inactive slot is
written and read back before the active selector changes. On boot, a pending
attempt whose proof key and MQTT credential match the active identity is treated
as already committed and cleaned up without repeating the claim exchange. This
reconciles a reset between the active-selector write and pending-record removal.
Factory provisioning
therefore leaves no partial identity when Wi-Fi, HTTPS, validation, or storage
fails. Existing devices can enter setup mode by holding the configured button for
three seconds; submitting a blank claim code changes Wi-Fi atomically while
retaining the prior working identity until the new network connects. Calling
`factoryReset()` explicitly removes both slots and re-enters setup.

Production storage requires ESP32 flash/NVS encryption. Startup fails closed when
flash encryption is absent. Local boards may opt in with
`allowUnencryptedNvsForLocalDevelopment`; the examples keep this off. Likewise,
compiled developer fixtures exist only inside
`NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE`, which defaults to `0`.

### CLI serial provisioning

`NivaloProvisioning` also owns the versioned USB/UART boundary used by the
Nivalo CLI. It listens on `Serial` by default (or the `Stream` selected through
`NivaloProvisioningConfig::cliSerial`) without coupling requests to diagnostic
output:

- `nivalo.cli.identify.v1` returns the factory MAC address and hardware ID;
- `nivalo.cli.claim.v1` accepts that exact hardware ID, one bounded Wi-Fi
  object, and an eight-character one-use code matching
  `tests/cli_claim_request_v1.json`. It reuses the captive portal's
  authoritative HTTPS proof flow: the ESP32 creates and durably stages its
  P-256 proof and MQTT credential, performs the certificate-validating HTTPS
  exchange and MQTT TLS verification itself, and acknowledges only after the
  A/B identity commit and pending-secret cleanup. Its success response contains
  only the request ID, hardware ID, device ID, and success state; proof keys and
  MQTT credentials never cross the serial boundary;
- `nivalo.cli.provision.v1` accepts one exact Wi-Fi object and seven-field MQTT
  TLS identity matching `tests/cli_provision_request_v1.json`, writes a
  durable recovery stage, commits to the inactive Preferences slot, verifies
  both the slot and active selector, replaces any older claim with a verified
  non-secret tombstone, clears the stage, and only then acknowledges and restarts;
- input is NDJSON bounded to 16 KiB, 256 bytes of work per loop, and a five-second
  frame lifetime; partial, malformed, oversized, extra-field, and unsupported
  requests fail closed;
- provisioning accepts only TLS ports `8883`/`8884`, a canonical device UUID,
  bounded identities, and an open or valid WPA passphrase. The request and all
  credential values are wiped from the line buffer and never echoed or logged.

After a successful claim, encrypted NVS retains only the code's SHA-256 receipt
with the active identity. If USB is lost after the commit but before the
terminal response, repeating the same serial request returns the committed
device ID without another HTTPS exchange. A different code cannot replace an
active identity through this factory serial path; use the explicit reset or
audited transfer workflow. The receipt is a local recovery discriminator, not
an authentication credential: it is never returned or logged, is accepted only
over the physical serial boundary for the exact hardware ID, and is never
stored when encrypted NVS is unavailable outside the explicit development
exception. A later Wi-Fi change or direct credential replacement invalidates
the receipt.

Boot recovery completes a durable CLI stage before it considers a prior captive-
portal claim. A reset or write failure at any transaction boundary therefore
leaves a retryable stage and cannot acknowledge an identity that the next boot
would silently replace.

Serial identification remains available when storage is unavailable, but serial
provisioning is rejected unless flash/NVS encryption is active or the existing
explicit local-development exception is enabled. A compiled local developer
fixture also disables serial provisioning because it would otherwise mask the
committed identity after restart. Set `enableCliSerial` to `false` for products
that do not expose this physical provisioning surface.

The claim wire contract is published in `nivalo-protocol/specs/device-claim-v1.md`.
The platform backend and console implement the matching organization-scoped
code issuance, QR/countdown display, bounded expiry and failure lockout,
code/IP/hardware rate limits, serializable one-use consumption, proof and replay
validation, audited hardware transfer, idempotent exact retry, one-way MQTT
credential verification, and password-free response contract. That platform
change still requires staging deployment and end-to-end acceptance with an
encrypted physical board before the claim flow can be described as live.

The CLI request fixture is also bound to an immutable `nivalo-platform` commit
by `tests/serial_claim_contract_pin.json`. CI runs the offline, deterministic
check below; it needs no cross-repository credential or network request:

```powershell
python tests/validate_serial_claim_contract.py
```

When intentionally advancing the platform contract, compare the ESP32 fixture
directly with the file at the pinned platform commit before updating the pin:

```powershell
python tests/validate_serial_claim_contract.py `
  --platform-repository ..\nivalo-platform
```

The optional command reads the pinned object with `git show`, so it neither
depends on nor changes the platform worktree's current branch. The platform
repository is private today; do not add an unauthenticated CI checkout or copy
a credential into this repository. A future live cross-repository checkout
must use a separate read-only deploy identity. The immutable pin remains the CI
authority until such an identity is provisioned.

MQTT uses certificate-validating TLS on production port `8883` or the isolated
staging port `8884`; no other provisioned broker port is accepted. Configure new
sketches through `NivaloMqttConfig`; the original positional `beginMqtt`
overload remains available for source compatibility and also uses TLS. The
built-in CA bundle trusts the current ISRG Root X1/X2 chains, and a custom
broker CA can be supplied with `caCertificate`.

Plain MQTT is only available through the explicit
`NIVALO_MQTT_TRANSPORT_PLAINTEXT_LOCAL` transport and is restricted to
single-label, `.local`/`.lan`, loopback, link-local, or RFC1918 broker hosts.
The library rejects plaintext for `mqtt.nivalo.io` and other public hosts. Do
not use `setInsecure`; certificate and hostname validation are part of the
default connection contract.

Application SDK dispatch, MQTT message publication, queued reports, UART
adaptation, connection policy, protocol helpers, OTA ownership, and status-pixel
state live in separate implementation units. `NivaloDevice` remains the public
facade, while mutable runtime state—including the NeoPixel driver—is owned by
each device instance.

NivaloLink invalid SPI frames are aggregated into at most one
`esp32.nivalolink.frame_invalid` event per minute after the initial report. The
event uses structured `data` with `error`, interval `count`, `dataReady`, and a
12-byte hexadecimal `rxPrefix`, so the platform retains actionable diagnostics
without persisting sampled raw MQTT envelopes or emitting one event per failed
poll.

After forwarding a command, the bridge waits for the secondary MCU's
`DATA_READY` signal before polling for response frames. The existing one-second
periodic poll remains the bounded fallback if that signal is unavailable.

Connection management is non-blocking: failed attempts use exponential backoff
from 1 to 60 seconds plus up to 25% random jitter. MQTT connects with a retained
QoS 1 offline Last Will. SNTP is started during configuration and the client
does not connect or publish envelopes until it can produce a real UTC `sentAt`;
the former 1970 fallback is gone. The loop registers/resets the ESP task watchdog
by default (30 seconds, configurable through `NivaloDeviceConfig`).

The retry timing policy is Arduino-free and has deterministic host coverage for
attempt gating, jitter bounds, the 60-second cap, success reset, and 32-bit
`millis()` rollover:

```sh
cmake -S tests/host -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

Set `mqtt.telemetryBufferCapacity` from 1 through 8 to enable the optional
in-memory bounded telemetry queue. Publish methods now return 1 only for an
actual MQTT publish; an offline sample may be buffered but returns 0. When full,
the oldest buffered sample is discarded. The queue is intentionally volatile.

`NivaloPinMap` replaces fixed SWD and NivaloLink SPI constants. Defaults preserve
the original board wiring, while products can supply their own map before begin.
Bridge builds send a payload-bounded, identifier-free NivaloLink HELLO at link
startup and every 30 seconds. This lets a secondary MCU re-establish protocol
identity after either side resets without waiting for a cloud command. The
existing 30-second heartbeat remains separate; both exchanges use the same
CRC-checked fixed frame transport.

## Signed firmware and recovery

Firmware flashing is fail-closed. Populate `firmwareSigningKeys` with ECDSA
P-256 SubjectPublicKeyInfo PEM values selected by exact `keyId`; an empty trust
set rejects every flash command. A command must contain the protocol-defined
`ecdsa-p256-sha256` signature over the exact canonical manifest. The device
verifies that signature, downloads to durable SPIFFS storage, then independently
checks the staged file's exact byte count and SHA-256 before `Update.begin` or
any STM32 erase/program operation. Missing signatures, unknown keys, malformed
DER/Base64, and target/size/hash/image tampering produce failed ACKs and events.

Provision only public keys on devices. For rotation, first deploy firmware that
trusts both the new current key and the immediately previous key, begin signing
artifacts with the new key after that firmware is established, and remove the
previous key only after the fleet migration window closes. Private signing keys
belong in the signing service's guarded key store, never in this repository,
device configuration, SPIFFS, or deterministic test fixtures. The checked-in
vectors use public-only, non-production fixture keys.

STM32 programming additionally requires one explicit recovery mode:

- `preserveKnownGoodStm32Image` plus the exact known-good image size reads the
  running target back to durable storage and verifies that snapshot before erase.
- `stm32GoldenImagePath`, size, and SHA-256 select a pre-provisioned golden image;
  the file is verified before erase.

Every STM32 update receives a full byte-for-byte SWD read-back. A program or
verification failure automatically restores the recovery image, verifies the
restore, and reports whether recovery succeeded. If recovery is not configured
or cannot be prepared, programming is refused.

Storage capacity is a deployment constraint, not a compile-time assumption.
ESP32 updates need free SPIFFS space at least equal to signed `sizeBytes`.
Snapshot-based STM32 updates need the staged update plus the configured
known-good image size; golden mode needs both files to coexist. The library
checks current free space and file sizes and rejects safely when capacity is
insufficient. Select and validate an ESP32 partition layout for the largest
supported artifacts before deployment.

OTA owns its SWD adapter and uses an RAII session to restore NivaloLink/pin/HTTP
state on every return path. Network shutdown is explicit only at the transition
to ESP32 Update or exclusive SWD programming; all error/return cleanup is owned
by the session destructor. The unused 462 KB compiled bootloader header was
removed. Bridge builds use the repository-local maintained Adafruit DAP fork;
there is no dependency-source mutation or build-time patch.

The maintained fork is active for repository bridge builds under
`third_party/adafruit-dap-nivalo/`. It vendors official tag `1.8.3` with commit,
license, and per-file hash provenance, adds STM32 ID `0x421` in maintained
source, and uses explicit local dependency paths—never a build-time patch.
Repository builds pin Espressif32 7.0.1 and every external Arduino dependency
to an exact version; the framework-provided `SD` library remains local to the
pinned Espressif32 platform. ESP32 CI validates only this repository and the
pinned protocol contracts. The examples repository owns its cross-repository
builds against one immutable ESP32 commit, avoiding a circular CI dependency.

CI runs for pull requests and for pushes to `main`; feature-branch pushes are
not run separately from their pull requests. The Windows registry-portability
lane is limited to changes that can affect the packaged device or maintained
DAP archives. Manual dispatch remains a fail-safe full run.

No remote or package has been created. Until publication, external bridge
consumers must supply the fork explicitly rather than relying on registry
dependency resolution.

## Registry metadata

`library.json` (PlatformIO) and `library.properties` (Arduino Library Manager)
describe release 0.2.0 and its dependencies. They are ready for a later reviewed
publication workflow; this repository change does not publish externally. See
`docs/registry-release.md` for immutable tag rules, dry-run packaging, named
Actions credentials, Arduino readiness gates, and rollback/deprecation steps.

## Runtime Telemetry

Call `device.publishRuntimeTelemetry()` from the sketch loop to publish the portal's standard runtime vitals:

- `uptime_ms`
- `wifi_rssi`
- `heap_used`
- `heap_free`
- `heap_total`

Sketches should prefer this helper over hand-written telemetry names for common ESP32 health metrics.

## Application functions, variables, and commands

Standalone sketches can expose application behavior without a secondary MCU:

```cpp
int ledState = 0;

int setLed(String argument) {
  ledState = argument == "on" ? 1 : 0;
  return ledState; // negative means failed; non-negative means succeeded
}

NivaloFunctionMetadata setLedMetadata;
setLedMetadata.displayName = "Set LED";
setLedMetadata.description = "Turns the built-in LED on or off.";
setLedMetadata.argumentExample = "\"on\"";
setLedMetadata.dangerLevel = "operational";

device.function("setLed", setLed, setLedMetadata);
device.variable("ledState", &ledState);
```

Applications that already configure SNTP and a local timezone can keep
ownership of that clock setup by setting `mqtt.configureClock = false`.
Nivalo still publishes UTC timestamps from `time()` without changing the
application's timezone.

Register before MQTT connects. The library publishes protocol-valid function
and variable definitions automatically at connection time and republishes when
a registration is added while connected. Registered references must remain
valid for the lifetime of the device. Capacity is intentionally bounded to 8
functions and 12 variables; duplicate, empty, overlong, null, or excess
registrations return `false`.

The metadata overload is additive; existing `function(name, handler)` sketches
keep the default integer return type, 30-second timeout, safe danger level, and
registration-order sorting. Supported danger levels are `safe`, `operational`,
`disruptive`, and `dangerous`. Metadata is copied during registration, so the
input structure does not need to remain alive afterward.

`device.publishVariables()` snapshots every registered reference to telemetry;
the standalone example calls it periodically. Supported references are signed
and unsigned `int`/`long`, `float`, `double`, `bool`, and Arduino `String`.

A named `function` runs before any bridge fallback. The library publishes the
accepted ACK on receipt and a succeeded/failed ACK after the handler returns. A
generic handler can cover commands that are not named functions:

```cpp
NivaloCommandResult handleCommand(String name, String arguments, String &message) {
  if (name != "calibrate") return NIVALO_COMMAND_UNHANDLED;
  message = "Calibration complete";
  return NIVALO_COMMAND_SUCCEEDED;
}

device.onCommand(handleCommand);
```

Returning `NIVALO_COMMAND_UNHANDLED` preserves the existing behavior: bridge
builds forward the command to NivaloLink; standalone builds publish a failed
unsupported-command ACK. The API uses the existing command, definitions, and
ACK schemas and introduces no wire-format variant.

Completed standalone function and generic-handler results are cached by
platform command ID in a bounded eight-entry FIFO. If the gateway redelivers an
ID, the device republishes the cached terminal ACK without executing the
application handler again. Reusing an ID for a different command fails closed.
The cache is intentionally volatile: an ESP32 restart clears it.

## Command ACK compatibility

Command acknowledgements use the canonical
`nivalo/v1/devices/{deviceId}/commands/{commandId}/ack` topic. Until the
protocol migration window closes on 2027-01-31 UTC, the library also publishes
the identical envelope to `nivalo/v1/devices/{deviceId}/acks/{commandId}`.
Consumers deduplicate the two deliveries by `messageId`. Define
`NIVALO_PUBLISH_LEGACY_COMMAND_ACKS=0` only when every connected consumer accepts
the canonical topic.

When a cloud command includes `requestedBy`, bridge builds preserve it in the
NivaloLink command sent to the secondary MCU. This value is attribution only,
not an authorization credential.
