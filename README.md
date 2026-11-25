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
provisioning starts SNTP and waits for a valid clock using bounded attempts
with backoff; claim HTTPS and MQTT identity validation fail closed until then.
"Valid" is the device clock of the MQTT session (an SNTP synchronisation in this
boot, at most 6 h old, no unrecovered backward step), shared by provisioning,
the session and OTA; a plausible wall clock retained over a warm restart does
not count.
Before its first HTTPS request, the device generates an ECDSA P-256 proof key
and a 32-byte MQTT credential and persists the entire pending attempt in
Preferences (see "Credential storage" below).
The request uses certificate-validating HTTPS and redirects are disabled. Claim codes,
passwords, response bodies, and signatures are never printed.

The response contains only a non-secret device/MQTT identity; the MQTT password
never leaves the device. The body is read into a fixed 3072-byte buffer and
must match `device-claim-response.schema.json` exactly (canonical lowercase
device UUID, DNS host, printable-ASCII client ID and user name of at most 128
bytes, CA of at most 2048 bytes) before anything is probed or promoted
(`NivaloClaimResponse`, which copies every value out of the buffer before it is
wiped and freed). A lost response or reset reloads and retries the exact
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
The pending attempt is read with the native NVS API: only "not found" counts as
no attempt. A pending record that cannot be read is never taken as absent or
cleared; the unit runs only if its active identity's protocol state already
reads back, and otherwise reports an error until a restart can read it (or the
setup button starts a new claim). No recovery acknowledgement and no
pending-record cleanup happen before the identity's journal and boot counter
read back.
Factory provisioning
therefore leaves no partial identity when Wi-Fi, HTTPS, validation, or storage
fails. Existing devices can enter setup mode by holding the configured button for
three seconds; submitting a blank claim code changes Wi-Fi, keeping the prior
identity, once the new network connects and the clock is valid. The identity
itself is then checked by the device's own MQTT session (a separate probe with
the same client ID would displace it). Calling `factoryReset()` explicitly
removes both slots and re-enters setup.

A commissioned unit that loses its saved WLAN for 20 seconds (at startup or
later), or cannot get a valid clock, opens the setup access point for a Wi-Fi
change and keeps the station running (AP+STA): the saved WLAN is retried after
30 s, then with doubling waits up to 5 minutes (`wifiRetryBaseMs`,
`wifiRetryMaximumMs`). Once it connects, the portal closes and the unit
continues as at startup. Without a setup code the retries run without the
access point. The setup button opens the portal without these retries.
The portal's web server listens only on the access point's address
(`192.168.4.1`) and serves only clients from the access point's subnet, so the
station's LAN cannot reach it. The wildcard captive DNS runs only in AP-only
setup; during an outage open `http://192.168.4.1` directly.

Committing a newly claimed identity also prepares its protocol state: a fresh
command journal (`highWater` 0) for the new device ID and, if missing, the boot
counter. The pending claim is kept until both the credentials and that state
are durable and read back, and an interrupted commit is finished at boot.
Wi-Fi changes, credential resets and `factoryReset()` keep the setup code, the
OTA floor, the journal and the boot counter (section 15 of the spec).

Only a claim (a new device identity) initializes the journal and the boot
counter. Manually supplied USB credentials (`nivalo.cli.provision.v1`) only
update the device ID whose journal and boot counter read back on this unit,
keeping its high-water mark and boot counter. Anything else (an empty unit, such
as a replacement board given a previously used ID; a missing or unreadable
journal or boot counter; another device ID's history) is refused before anything
changes, so the device stays blocked rather than replaying an operation. Claim
such a unit instead. The compiled developer fixture
(`NIVALO_ENABLE_LOCAL_DEVELOPER_FIXTURE`, off in every stock profile) may also
initialize a unit with no protocol state at all; if its own journal and boot
counter were both lost it seeds them again, so never ship a fixture build.

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
the code is never echoed. Sending a new code rotates it: a running access point
is stopped (its stations are disconnected) and reopened with the new code before
the response is sent, so the old passphrase stops working at once. When the
access point cannot start, AP mode is switched off rather than left in an
earlier configuration.

**Limitation:** the portal itself stays on the Arduino stack and serves plain
HTTP on the isolated setup network. WPA2 with the setup code is the protection;
anyone who holds the code is inside the setup trust boundary. Secure boot, flash
and NVS encryption and a stronger provisioning channel are later ESP-IDF work.

### Credential storage

The Wi-Fi password, the MQTT credentials, the claim proof private key, the
pending claim and the setup code are stored with Preferences (NVS).
`NivaloProvisioning` uses it only where NVS encryption is actually active (NVS
encryption with an `nvs_keys` partition under flash encryption; the flash
encryption eFuse alone does not count), unless `allowPlaintextNvs` is set.
`storageProtected()` reports which one is in use.

**The owner's boards use plain NVS.** They are stock original ESP32 Feathers
without flash encryption, and the Arduino-ESP32 2.0.17 prebuilt SDK cannot
encrypt NVS at all, so the `Esp32Only` staging and production environments
build with `NIVALO_ALLOW_PLAINTEXT_NVS=1`. Anyone with physical access to a unit
can read the Wi-Fi password, the MQTT credentials and the claim proof key from
its flash. Recovery from a lost or tampered unit: revoke (retire) the device in
the platform, which disables its credential, and re-claim the hardware, which
issues a new identity; change the Wi-Fi password if it may have been read. Flash
and NVS encryption come with the later ESP-IDF re-platform.

Compiled developer fixtures exist only inside
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
  non-secret tombstone, confirms that device ID's protocol state reads back (see
  the identity rules above; a request those rules refuse changes nothing),
  clears the stage, and only then acknowledges and restarts;
- input is NDJSON bounded to 16 KiB, 256 bytes of work per loop, and a five-second
  frame lifetime; partial, malformed, oversized, extra-field, and unsupported
  requests fail closed;
- provisioning accepts only TLS ports `8883`/`8884`, a canonical device UUID,
  bounded identities, and an open or valid WPA passphrase. The request and all
  credential values are wiped from the line buffer and never echoed or logged.

After a successful claim, NVS retains only the code's SHA-256 receipt
with the active identity. If USB is lost after the commit but before the
terminal response, repeating the same serial request returns the committed
device ID without another HTTPS exchange. A different code cannot replace an
active identity through this factory serial path; use the explicit reset or
audited transfer workflow. The receipt is a local recovery discriminator, not
an authentication credential: it is never returned or logged, is accepted only
over the physical serial boundary for the exact hardware ID, and is never
stored when NVS is unavailable to provisioning (no NVS encryption and no
`allowPlaintextNvs`). A later Wi-Fi change or direct credential replacement invalidates
the receipt.

Boot recovery completes a durable CLI stage before it considers a prior captive-
portal claim. A reset or write failure at any transaction boundary therefore
leaves a retryable stage and cannot acknowledge an identity that the next boot
would silently replace.

Serial identification remains available when storage is unavailable, but serial
provisioning is rejected unless NVS encryption is active or `allowPlaintextNvs`
is set. A compiled local developer
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
and then sends DISCONNECT; if that end message could not be published it closes
the connection without DISCONNECT, so the broker publishes the Will instead.
Heartbeats are free; a device needs no runtime telemetry for health. The Will
is built in the device's own buffer, and the MQTT packet buffer (4 KiB) and
every copied setting are checked before the boot counter advances, so a start
that runs out of memory changes nothing and can be retried (the example retries
every 30 s).

Connection management is non-blocking between attempts: failed attempts use
exponential backoff from 1 to 60 seconds plus up to 25% random jitter. One
attempt blocks the loop, bounded by a 5 s TCP connect, a 10 s TLS handshake and
a 5 s MQTT socket timeout, plus up to 15 s of DNS that the Arduino core does not
let us shorten; an OTA request adds an 8 s response timeout. The loop
registers/resets the ESP task watchdog by default (60 seconds, at least 50,
configurable through `NivaloDeviceConfig`) and feeds it before each blocking
write; a `begin()` that fails unsubscribes the task again.

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
heap figures as one sample. An invalid reading (bad name, unit or value, or a
name already in the sample, case twins included) is refused and leaves the
pending sample unchanged; only a full batch is published to make room. A
telemetry message that cannot be copied into the offline buffer counts as
dropped. Values are exact decimals with at most six
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

The arguments are the exact bytes received, parsed once into the object that
the validator and then the handler see; nothing is re-serialized in between, so
a value such as `0.9999999999` reaches the handler as sent. Integers keep their
exact value; every other number is the correctly rounded `double` of its
original token (`strtod`), not ArduinoJson's approximation, so a validator that
allows at most 1 also refuses the exact decimal of 1 + 2⁻⁵². A number that object
cannot represent (non-finite, an integer outside 64 bits, a nonzero value that
underflows to zero) is refused as `invalid_command` before the claim, like
`null`. The validator runs before the operation is claimed (`false` rejects it
as `invalid_command`). The handler and its arguments are prepared before the
claim, and the completion token and argument view are armed before the final
clock and deadline reading, so that reading is immediately followed by the
handler call; a rejection by that reading withdraws the token. The handler is
called at most once and returns:

| Outcome | ACK |
|---|---|
| `NIVALO_FUNCTION_SUCCEEDED` | `succeeded` |
| `NIVALO_FUNCTION_FAILED` (the effect certainly did not happen) | `failed` / `handler_failed` |
| `NIVALO_FUNCTION_UNKNOWN` (the effect may have happened, e.g. a timeout after an output changed) | `indeterminate` / `execution_unknown` |
| `NIVALO_FUNCTION_PENDING` (still running; report the outcome later with `device.completeFunction(token, outcome)`) | `running` to duplicates until then |

A handler that returns `NIVALO_FUNCTION_PENDING` reads its invocation token with
`device.functionToken()` while it runs. `completeFunction(token, outcome)` may
be called from any task: it only queues the outcome, and `device.loop()`
journals it on the executor; the device stays busy until that terminal result
is durable. A stale or unknown token (an earlier invocation, a duplicate
completion) is refused and changes nothing.

Handlers run from `device.loop()` and must not call it. `requestDefinitions` is
built in. Reserved names (`flash`, `requestDefinitions`, `firmware.*`, `ota.*`
in any case) and case twins of one kind are refused. Capacity is 8 functions
and 12 variables; supported variable types are signed and unsigned `int`/`long`,
`float`, `double`, `bool`, and Arduino `String`.

**Catalog.** Register before the first connection. At the first connection the
library compares the registered definitions with the catalog it reported last
(revision, digest and names are kept in NVS): an unchanged catalog keeps its
revision, any changed definition, rename or removal advances it and is stored
before it is published. Removed names are kept as tombstones (at most 16) and
re-sent as removes; a case-only rename always travels as its remove and upsert
pair. A change that cannot be represented, more than 16 retired names or a
live name with two retired case twins (renaming one name twice by case only),
is refused: nothing is stored or published, the stored history stays, and the
device logs it. A stored catalog that cannot be read is never treated as a
first enrollment or written over; the refresh is retried while connected, as is
any refresh that did not complete. After every birth and on
`requestDefinitions` the whole catalog is published in packets of at most 8
operations; `requestDefinitions` succeeds only when every packet was
published (`failed`/`handler_failed` when none was, `indeterminate` when some
were). Registrations after that are refused.

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
downlinks until it is re-claimed (`device.needsReclaim()`). Envelope `firmware`
metadata on an operation route is `invalid_command` (firmware placement).

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
security version must match, and `esp_ota_end()` validates it. The image must
contain the descriptor magic exactly once at any byte offset, overlapping
occurrences included. The download starts only while the clock is valid and the
deadline allows it, and stops as soon as a fresh reading shows the job can no
longer be activated (`expired` or `clock_unsynced`). A pending record
(sequence, product, version, size, digest) is written and read back, the clock
and deadline are re-checked on a reading taken after that write, and only then
is `esp_ota_set_boot_partition()` called, exactly once. A definite refusal is a
zero-effect `ota_rejected`; an uncertain failure keeps the claim and reboots.
At boot, before connecting, a claimed OTA record with a matching pending record
is reconciled by hashing exactly `sizeBytes` bytes of the running slot: on a
match the floor is raised to at least the installed version (a higher floor is
kept) and read back, then `succeeded` is journaled, then the pending record is
cleared; otherwise (a different image, or a pending record that is missing or
damaged) `indeterminate` / `execution_unknown`. The pending record is only ever
cleared after the terminal result is durable. When the pending record cannot be
read, or the installed image runs but the floor cannot be read, the claim and
the pending record stay as they are and the reconciliation is retried every
10 s; meanwhile the job is answered `running` and nothing new starts. A
committed floor record that cannot be read disables OTA (every job is
`ota_rejected`) and is left in place for recovery; it is never taken as "no
floor". The protocol state uses the native NVS API, where only "not found"
means absent; any other read error is "unreadable".

Populate `firmwareSigningKeys` with ECDSA P-256 SubjectPublicKeyInfo PEM values
selected by exact `keyId`; an empty trust set rejects every OTA job. The
`Esp32Only` example carries the release public key of each environment in the
tracked `include/nivalo_firmware_trust.h` (production `nivalo-prod-2026-11`,
staging `staging-d364850176bd64cd`), selected by `NIVALO_ENVIRONMENT_STAGING` or
`NIVALO_ENVIRONMENT_PRODUCTION` in `platformio.ini`. Both slots, current and
previous, come from that file: a staging or production build without its key,
with either slot defined anywhere else (`nivalo_config.h`, a build flag), or
with a `test-` key ID fails to compile. The public key is the SubjectPublicKeyInfo of
`Nivalo.<IoT|Staging>.Firmware.ArtifactSigning.CurrentPrivateKeyPkcs8Base64`,
derived in memory (as `nivalo-platform/infrastructure/nivalo-vm/Write-NivaloEnvFiles.ps1`
does for the console). Provision
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
  model (`tests/protocol_v1_1.py`) by `tests/validate_v11_uplinks.py`;
- `nivalo_v11_store_tests`: the real `NivaloDeviceStore` on a map-backed
  stand-in for the native NVS API (`tests/host/fakes`) with injectable lookup
  and read errors: what a credential change may do to the journal and the boot
  counter, and unreadable floor, journal, pending and catalog records;
- `nivalo_v11_claim_tests`: claim exchange responses through
  `NivaloClaimResponse` (validation and credential extraction, with the
  response buffer wiped before any value is read).

The execution tests also cover asynchronous completion tokens, correctly
rounded argument numbers, the order of arming, the final clock reading and the
handler call, pending-record cleanup after a failed or interrupted terminal
write, and boot reconciliation while the pending record or the floor cannot be
read.

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
and cached PlatformIO builds of the standalone example (`featheresp32` staging
and `featheresp32-production`, each with its trust anchor) and of the bridge
(`featheresp32`). The browser-provisioning, NodeMCU and Arduino-sketch builds, the factory-image inspection and the
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
