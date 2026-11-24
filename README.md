# Nivalo ESP32

ESP32 Arduino/PlatformIO library for Nivalo devices. It implements Nivalo cloud
MQTT **v1.1** (`nivalo-protocol` `specs/cloud-mqtt-v1.1.md`); protocol v1 is
gone, with no dual-read.

The explicit original-ESP32 browser-provisioning artifact build and its
independent partition fingerprint procedure are documented in
[`docs/browser-flash-artifact.md`](docs/browser-flash-artifact.md).

`NivaloDevice` is the primary API for both standalone ESP32 devices and ESP32 devices that bridge to a secondary MCU. Secondary MCU support is enabled at build time with `NIVALO_HAS_SECONDARY_MCU=1`.

## Layout

```text
src/
  NivaloDevice.h / NivaloDevice*.cpp   Arduino facade: session, uplinks, downlink inbox, OTA install
  NivaloDeviceStore.*                  durable protocol state (NVS namespace nivalo-dev)
  NivaloProvisioning.*                 setup access point, claim, CLI serial provisioning
  NivaloConnection.*                   MQTT/TLS connection and reconnect policy
  -- Arduino-free protocol units, compiled unchanged by the host tests --
  NivaloWire.*        limits, UTC, UUID, names, text, JSON writer, topics
  NivaloJsonStrict.*  strict JSON scanner (encoding, duplicates, depth, value count)
  NivaloDownlink.*    commands/{id} and ota/{id} receiver and schema checks
  NivaloClock.*       clock validity and the monotonic operation deadline
  NivaloExecutor.*    the single executor and its journal transitions
  NivaloJournal.*     journal, pending and floor records and their blob encodings
  NivaloOtaPolicy.*   manifest signature, prechecks, image descriptor, reconciliation
  NivaloImageIdentity.cpp  this image's product ID and security version
  NivaloCatalog.*     capability revision, tombstones and refresh packets
  NivaloUplink.*      envelope, availability, ACK, telemetry batch, event, state builders
  NivaloLink*, NivaloOta.*, NivaloStatusPixel.*  bridge transport, pins, status LED
examples/
  Esp32Only/
  Esp32Stm32Bridge/
  NivaloRuntimeProvisioning/
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
settings. `begin` returns `false` when the device cannot run a session, for
example when its boot counter is missing (re-claim the device).

### Product identity, staging and production

Each image carries one product ID and security version, set per environment at
build time:

```ini
build_flags =
    -DNIVALO_PRODUCT_ID=\"<product UUID>\"
    -DNIVALO_SECURITY_VERSION=<1..2147483647>
    -DNIVALO_API_ORIGIN=\"https://iot-api.nivalo.io\"
```

Staging and production use **different product IDs**, so an image signed for one
environment never installs on a device of the other. The `Esp32Only` example
builds staging in `featheresp32` (product `064c6fb6-a7f6-4646-87e3-5df87e8634f1`,
`https://iot-api-staging.nivalo.io`) and production in `featheresp32-production`
(product `66f13a6e-6d3c-48ef-845c-86f18297e55b`, `https://iot-api.nivalo.io`).
These product IDs must be registered with the matching platform environment.
Raise `security_version` in `platformio.ini` with every release.

The product ID the device accepts is the one compiled into its running image;
moving a unit between environments takes a USB flash of the other
environment's image. Without `NIVALO_PRODUCT_ID` (for example an Arduino IDE
build) every OTA job is rejected.

`NivaloMqttConfig::apiOrigin` names the API origin (`https://host[:port]`).
Firmware is downloaded only from that origin; without it every OTA job is
rejected.

## Runtime provisioning and claiming

`NivaloProvisioning` removes compiled production Wi-Fi and MQTT credentials.
On first boot it starts a `Nivalo-Setup-xxxxxx` access point protected by the
unit's setup code (below), a wildcard DNS captive portal and a non-blocking web
server. The user submits Wi-Fi plus an eight-character
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

Committing a newly claimed identity also prepares its protocol state: a fresh
command journal (`highWater` 0) for the new device ID and, if missing, the boot
counter. Wi-Fi changes, credential resets and `factoryReset()` keep the setup
code, the OTA floor, the journal and the boot counter (section 15 of the spec).

### Setup access point and setup code

Each unit has one owner-held setup code: 16 random uppercase Crockford Base32
characters (80 bits), printed on the unit's private card. It is the **WPA2
passphrase** of the setup access point and nothing else: it never appears in
MQTT, claim requests, logs or any cloud API. The access point admits one
station. There is no open access point: without a provisioned code the portal
does not start (`lastError()` says so) and the unit can still be set up over USB.

The code is provisioned over USB into the factory namespace `nivalo-fact`, which
no reset clears:

```json
{"schema":"nivalo.cli.setup-code.v1","requestId":"<32 hex>","expectedHardwareId":"esp32-...","setupCode":"<16 Crockford Base32>"}
```

The response is `{"schema":"nivalo.cli.setup-code.v1","requestId":"...","ok":true}`;
the code is never echoed. Sending a new code rotates it.

**Limitation:** the portal itself stays on the Arduino stack and serves plain
HTTP on the isolated setup network. WPA2 with the setup code is the protection;
anyone who holds the code is inside the setup trust boundary. Secure boot, flash
and NVS encryption and a stronger provisioning channel are later ESP-IDF work.

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
- `nivalo.cli.setup-code.v1` stores the unit's setup code (above);
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

## MQTT session (protocol v1.1)

All topics are `nivalo/v1.1/devices/{deviceId}/{route}`: uplinks `telemetry`,
`events`, `state`, `definitions`, `availability`, `commands/{id}/ack` and
`ota/{id}/ack`; downlinks `commands/{id}` and `ota/{id}`, subscribed at QoS 1.
Every envelope is `nivalo.iot.v1.1` with a fresh `messageId` and a `sentAt` from
the device clock. Every message the device builds is re-checked with the strict
scanner and the 3072-byte limit before it is published.

MQTT uses certificate-validating TLS on production port `8883` or the isolated
staging port `8884`; no other provisioned broker port is accepted. Configure new
sketches through `NivaloMqttConfig`. The built-in CA bundle trusts the current
ISRG Root X1/X2 chains, and a custom broker CA can be supplied with
`caCertificate`. The connection is MQTT 3.1.1 with a clean session and a 60 s
keepalive.

Plain MQTT is only available through the explicit
`NIVALO_MQTT_TRANSPORT_PLAINTEXT_LOCAL` transport and is restricted to
single-label, `.local`/`.lan`, loopback, link-local, or RFC1918 broker hosts.
The library rejects plaintext for `mqtt.nivalo.io` and other public hosts. Do
not use `setInsecure`; certificate and hostname validation are part of the
default connection contract.

**Clock.** The clock is valid only after an SNTP synchronisation in this boot,
at most 6 h old by the monotonic timer, with no backward step of more than 2 s
since (it becomes valid again when a synchronisation reaches the last trusted
instant). UTC is the last trusted synchronisation plus monotonic time. The
library observes every SNTP synchronisation from before `setup()`; an
application must not install its own SNTP notification callback. Without a
valid clock the device opens no TLS/MQTT connection and sends nothing.

**Presence.** A persisted boot counter is advanced once per boot, before any
connection; a RAM connection counter is advanced before every CONNECT attempt.
`(boot, connection)` is the session epoch. Before each CONNECT the Last Will is
rebuilt (fresh envelope, `end`/`will`, QoS 1, retained). After CONNECT both
SUBSCRIBE packets must be written (PubSubClient cannot observe SUBACK);
otherwise the device disconnects and retries. Then it publishes the retained
`birth` (with its firmware identity and `targets: ["esp32"]`), refreshes its
capability catalog and repeats the last terminal ACK. While connected it sends
a non-retained `heartbeat` every 600 ± 30 s with uptime, heap, RSSI and dropped
telemetry batches. `device.shutdown()` publishes the retained `end`/`shutdown`
and disconnects. Heartbeats are free; a device needs no runtime telemetry for
health.

Connection management is non-blocking: failed attempts use exponential backoff
from 1 to 60 seconds plus up to 25% random jitter. The loop registers/resets the
ESP task watchdog by default (30 seconds, configurable through
`NivaloDeviceConfig`).

## Telemetry, events and state

Telemetry is batched: one message carries up to 8 samples and 24 readings, each
sample observed at its own time. Usage is counted per 16 readings plus one per
event, so batching costs nothing extra.

```cpp
device.addTelemetry("temp", 21.125, "C");     // opens a sample at the first reading
device.addTelemetry("humidity", 41.5, "%");
device.nextTelemetrySample();                // the next reading starts a new sample
device.addTelemetryRaw("door", "open");
device.flushTelemetry();                     // publishes the batch
```

A full batch is published automatically. `publishVariables()` publishes every
registered variable as one sample, `publishTelemetry(name, value, unit)` one
reading at once, and `publishRuntimeTelemetry()` `uptime_s`, `wifi_rssi` and
heap figures as one sample. Values are exact decimals with at most six
fractional digits within ±4294967295.999999; names, units and raw text follow
the protocol's limits and are refused otherwise. Publish methods return 1 only
for an actual MQTT publish.

Set `mqtt.telemetryBufferCapacity` from 1 through 8 to keep telemetry messages in
RAM while offline. Buffered messages keep their bytes (`messageId`, `sentAt`);
when full, the oldest is discarded and counted in the heartbeat's
`droppedBatches`. The buffer is volatile.

`publishEvent(name, data, severity)` sends `data` as structured JSON when it is
strict JSON and as `raw` text otherwise; `publishState(name, valueJson)` sends a
state value. The `state.` event prefix is reserved.

## Application functions and variables

Functions take a JSON object of arguments and report what happened:

```cpp
bool ledState = false;

bool validSetLed(JsonObjectConst arguments)
{
  return arguments.size() == 1U && arguments["state"].is<bool>();
}

NivaloFunctionOutcome setLed(JsonObjectConst arguments)
{
  ledState = arguments["state"].as<bool>();
  digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
  return NIVALO_FUNCTION_SUCCEEDED;
}

NivaloFunctionMetadata metadata;
metadata.displayName = "Set LED";
metadata.argumentExample = "{\"state\":true}";
metadata.returnType = "void";
metadata.dangerLevel = "operational";
metadata.validateArguments = validSetLed;

device.function("setLed", setLed, metadata);
device.variable("ledState", &ledState);
```

The validator runs before the operation is claimed (`false` rejects it as
`invalid_command`); arguments never contain `null`. The handler is called at
most once and returns:

| Outcome | ACK |
|---|---|
| `NIVALO_FUNCTION_SUCCEEDED` | `succeeded` |
| `NIVALO_FUNCTION_FAILED` (the effect certainly did not happen) | `failed` / `handler_failed` |
| `NIVALO_FUNCTION_UNKNOWN` (the effect may have happened, e.g. a timeout after an output changed) | `indeterminate` / `execution_unknown` |
| `NIVALO_FUNCTION_PENDING` (still running; call `device.completeFunction(outcome)` later) | `running` to duplicates until then |

Handlers run from `device.loop()` and must not call it. `requestDefinitions` is
built in. Reserved names (`flash`, `requestDefinitions`, `firmware.*`, `ota.*`
in any case) and case twins of one kind are refused. Capacity is 8 functions
and 12 variables; supported variable types are signed and unsigned `int`/`long`,
`float`, `double`, `bool`, and Arduino `String`.

**Catalog.** Register before the first connection. At the first connection the
library compares the registered definitions with the catalog it reported last
(revision, digest and names are kept in NVS): an unchanged catalog keeps its
revision, any changed definition, rename or removal advances it and is stored
before it is published. Removed names are kept as tombstones and re-sent as
removes; a case-only rename always travels as its remove and upsert pair. After
every birth and on `requestDefinitions` the whole catalog is published in
packets of at most 8 operations. Registrations after that are refused.

## Command execution

Generic commands and OTA jobs share one per-device sequence and one durable
journal: the highest consumed sequence and the record of that operation (ID,
class, SHA-256 of the exact payload bytes, phase, result), one NVS blob that is
read back after every write. All journal transitions run on one executor in
`loop()`; the MQTT callback only validates, fingerprints and copies the
downlink while PubSubClient's buffer is intact.

| Delivery | Device action |
|---|---|
| sequence below the high-water mark | `indeterminate` / `replay_suppressed` |
| same sequence, ID, class and payload bytes | `running` while busy, else the journaled result |
| same sequence, anything different | `failed` / `id_conflict` |
| higher sequence while busy | dropped, not consumed |
| higher sequence | validate, check the clock, journal `claimed`, ACK `accepted`, re-check the clock and the monotonic deadline, run once, journal and ACK the result |

The deadline is derived at the claim from `expiresAt` and only ever made earlier
by later trusted synchronisations; expiry is checked on a fresh reading right
before the effect. A `claimed` generic record found at boot becomes
`indeterminate` / `execution_unknown`. The last terminal ACK is repeated after
every birth and with every heartbeat from RAM (no NVS write per heartbeat). If
the journal is missing, corrupt or cannot be written, the device accepts no
downlinks until it is re-claimed (`device.needsReclaim()`).

## Firmware updates (OTA)

OTA jobs arrive on `ota/{id}` with a format-2 manifest. Before any download the
device checks, in order: the `keyId` is in the compiled trust store, the ECDSA
P-256 signature over
`NIVALO-FIRMWARE-SIGNATURE-V2\n{target}\n{productId}\n{securityVersion}\n{sizeBytes}\n{sha256}\n`,
the product ID equals this image's, the security version is above
`max(running image's security version, committed floor)`, the size fits the
inactive slot, and the URL's origin equals `apiOrigin` (scheme, host, effective
port).

The image then streams over certificate-validating HTTPS without redirects
straight into the inactive slot; exactly `sizeBytes` bytes (an exact
Content-Length is required), the SHA-256 and the embedded product ID and
security version must match, and `esp_ota_end()` validates it. A pending record
(sequence, product, version, size, digest) is written and read back, the clock
and deadline are re-checked on a reading taken after that write, and only then
is `esp_ota_set_boot_partition()` called, exactly once. A definite refusal is a
zero-effect `ota_rejected`; an uncertain failure keeps the claim and reboots.
At boot, before connecting, a claimed OTA record with a matching pending record
is reconciled by hashing exactly `sizeBytes` bytes of the running slot: on a
match the floor is committed, then `succeeded` is journaled, then the pending
record is cleared; otherwise `indeterminate` / `execution_unknown`.

Populate `firmwareSigningKeys` with ECDSA P-256 SubjectPublicKeyInfo PEM values
selected by exact `keyId`; an empty trust set rejects every OTA job. Provision
only public keys on devices. For rotation, first deploy firmware that trusts
both the new current key and the immediately previous key, begin signing
artifacts with the new key after that firmware is established, and remove the
previous key only after the fleet migration window closes. Private signing keys
belong in the signing service's guarded key store, never in this repository,
device configuration or test fixtures; release key IDs never start with
`test-`.

This is application-level downgrade protection. Physical flash access, NVS
erase and bootloader bypass are outside it; secure boot, flash encryption and
hardware rollback are later ESP-IDF work.

## Secondary MCU bridge

Bridge builds (`NIVALO_HAS_SECONDARY_MCU=1`) keep compiling and forward the
MCU's telemetry and events over NivaloLink and UART as v1.1 uplinks. They do not
advertise MCU execution or STM32 OTA in v1.1: the birth lists only `esp32`,
commands run only on registered ESP32 functions, and MCU definitions and
command status frames are not reported. STM32 SWD programming was removed from
the command path (it remains in history at `4e0368d`,
`src/NivaloDeviceOta.cpp`); the bridge profile and its DAP defects are tracked
in issue #31.

NivaloLink invalid SPI frames are aggregated into at most one
`esp32.nivalolink.frame_invalid` event per minute after the initial report.
After forwarding a command, the bridge waits for the secondary MCU's
`DATA_READY` signal before polling for response frames. Bridge builds send a
payload-bounded, identifier-free NivaloLink HELLO at link startup and every 30
seconds.

`NivaloPinMap` replaces fixed SWD and NivaloLink SPI constants. Defaults preserve
the original board wiring, while products can supply their own map before begin.
Bridge builds use the repository-local maintained Adafruit DAP fork under
`third_party/adafruit-dap-nivalo/`. It vendors official tag `1.8.3` with commit,
license, and per-file hash provenance, adds STM32 ID `0x421` in maintained
source, and uses explicit local dependency paths—never a build-time patch.

## Host tests

The protocol units are Arduino-free and compiled unchanged for the host. The
tests replay `nivalo-protocol`'s `fixtures/v1.1` against them:

- `nivalo_v11_wire_tests`: every message in `messages.json` through the strict
  scanner and the downlink receiver, and the schema fixtures of every payload
  the device receives;
- `nivalo_v11_execution_tests`: `vectors/command-execution.json` (journal,
  deadline, clock validity, fingerprints, OTA install and boot reconciliation,
  power loss) through the real parser and executor;
- `nivalo_v11_ota_tests`: `vectors/ota.json` (canonical bytes, signatures and
  mutations, prechecks), the image descriptor and the record encodings;
- `nivalo_v11_catalog_tests`: a port of the reference merge proven against
  `vectors/capability-merge.json`, then fed the firmware's own refresh packets
  across releases (unchanged, changed, removed, renamed);
- `nivalo_v11_uplink_tests` and `nivalo_v11_uplink_reference_model`: every
  uplink the builders produce is replayed through the protocol's reference
  model (`tests/protocol_v1_1.py`) by `tests/validate_v11_uplinks.py`.

The presence, command-dispatch and ingest-metering vectors describe the server
and are not replayed here.

```sh
cmake -S tests/host -B build/host -DCMAKE_BUILD_TYPE=Release -DNIVALO_PROTOCOL_DIR=../nivalo-protocol
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

CMake downloads ArduinoJson 6.21.6 and mbedtls 2.28.7 (the version bundled
with Arduino-ESP32 2.0.17) by pinned SHA-256. The reference-model test needs
Python with `jsonschema==4.25.1` and `rfc3339-validator==0.1.4`.

CI is one Linux job for pull requests and pushes to `main`: the host CTest
suite against the pinned protocol commit (`NIVALO_PROTOCOL_REF`), the claim and
DAP provenance checks, the Python tooling tests, Arduino Lint (specification),
and cached PlatformIO builds of the standalone and bridge examples
(`featheresp32` environment of each). The browser-provisioning, production,
NodeMCU and Arduino-sketch builds, the factory-image inspection and the
clean-cache registry archive builds run in the `Registry release` workflow (see
`docs/registry-release.md`).

Repository builds pin Espressif32 7.0.1 and every external Arduino dependency
to an exact version; the framework-provided `SD` library remains local to the
pinned Espressif32 platform. The examples repository owns its cross-repository
builds against one immutable ESP32 commit, avoiding a circular CI dependency.

## Registry metadata

`library.json` (PlatformIO) and `library.properties` (Arduino Library Manager)
describe release 0.2.0 and its dependencies. They are ready for a later reviewed
publication workflow; this repository change does not publish externally. See
`docs/registry-release.md` for immutable tag rules, dry-run packaging, named
Actions credentials, Arduino readiness gates, and rollback/deprecation steps.
