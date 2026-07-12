# Nivalo ESP32

ESP32 Arduino/PlatformIO library for Nivalo devices.

The attended Phase-1 device acceptance procedure and evidence template are in
[`docs/phase1-hardware-drill.md`](docs/phase1-hardware-drill.md).

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
one-use claim code. Wi-Fi is tried for a bounded 20 seconds. Before its first
HTTPS request, the device generates an ECDSA P-256 proof key and a 32-byte MQTT
credential and persists the entire pending attempt in encrypted Preferences.
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

The claim wire contract is published in `nivalo-protocol/specs/device-claim-v1.md`.
The remaining backend/console work is deliberately not implemented here: create
and display organization-scoped codes, expire within ten minutes, atomically
consume once, lock after five failures, rate-limit by code/IP/hardware ID, reject
nonce replay/concurrent consumption, make exact retries idempotent while rejecting
mismatched replay, verify the canonical proof, enforce audited hardware transfers,
store only a one-way verifier for the device-generated MQTT credential, and return the
claim response schema without any reusable claim secret.

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

device.function("setLed", setLed);
device.variable("ledState", &ledState);
```

Register before MQTT connects. The library publishes protocol-valid function
and variable definitions automatically at connection time and republishes when
a registration is added while connected. Registered references must remain
valid for the lifetime of the device. Capacity is intentionally bounded to 8
functions and 12 variables; duplicate, empty, overlong, null, or excess
registrations return `false`.

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
