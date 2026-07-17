# Browser provisioning firmware artifact

The `featheresp32-browser-provisioning` PlatformIO environment is the only
firmware build in this repository labeled for the console's F11 browser
flasher. It targets an original Adafruit Feather ESP32 with an
`ESP32-D0WDQ6` and exactly 4 MiB of JEDEC flash. A C3, S2, S3, another original
ESP32 silicon variant, another carrier board, or another flash size requires a
separate reviewed descriptor and artifact; do not broaden this build.

The regular `featheresp32`, direct-flash `nodemcu-32s-phase1-bench`, and bridge
environments do not define `NIVALO_BROWSER_FLASH_ARTIFACT`, do not embed the
descriptor, and remain application-only PlatformIO outputs. In particular,
the NodeMCU bench build must never be offered through the browser-flash trust
map as if it were the separately described Feather artifact.

## Evaluation storage policy

A normally erased Feather has flash/NVS encryption disabled. The current
browser build is therefore deliberately an **evaluation artifact**, suitable
for the connected test board but not for customer or production credentials.
Only this environment pairs `NIVALO_BROWSER_FLASH_ARTIFACT=1` with
`NIVALO_BROWSER_FLASH_ALLOW_UNENCRYPTED_NVS_FOR_EVALUATION=1`. It reaches the
setup AP on a fresh board using the existing unencrypted local-development NVS
exception and identifies itself to the platform as
`adafruit-feather-esp32-browser-evaluation`.

The environment includes only the checked-in
`nivalo_browser_flash_config.h`; it never includes the ignored
`nivalo_config.h`. Developer fixtures are compile-time forbidden, so local
Wi-Fi or MQTT credentials cannot enter the artifact or bypass the setup AP. The
evaluation claim endpoint is the isolated staging API.

The application embeds the exact
`NIVALO-PROVISIONING-STORAGE-POLICY-V1:evaluation-unencrypted-nvs` marker. The
post-build inspector requires that marker exactly once and emits
`"credentialStoragePolicy":"evaluation-unencrypted-nvs"` with
`"productionEligible":false`. Removing the guard or marker fails the build;
ordinary builds retain the encrypted-storage fail-closed default.

A production browser artifact requires a separately reviewed encrypted-board
enablement path. Flash-encryption eFuses are irreversible and this repository
does not burn them automatically.

## Build and inspect

The pinned PlatformIO environment creates the unsigned application and then a
complete factory image automatically:

```powershell
pio run --project-dir examples/Esp32Only --environment featheresp32
pio run --project-dir examples/Esp32Only --environment featheresp32-browser-provisioning
python tests/validate_browser_flash_artifact.py
python scripts/browser_flash_artifact.py inspect `
  examples/Esp32Only/.pio/build/featheresp32-browser-provisioning/nivalo-provisioning.merged.bin
```

The merged file is
`nivalo-provisioning.merged.bin`. It contains the compatibility header at
`0x0`, the real original-ESP32 bootloader at `0x1000`, the exact 3072-byte
partition table at `0x8000`, the OTA selector initializer at `0xe000`, and the
provisioning application at `0x10000`. The compatibility header occupies only
the otherwise-unused first-sector prefix; original ESP32 boots the real
second-stage bootloader at `0x1000`. Inspection also requires every byte from
`0x8c00` through `0xdfff` to remain erased, including the complete fresh-board
NVS region beginning at `0x9000`; an artifact cannot preload credentials.

The same build writes `nivalo-provisioning.merged.evidence.json`. The inspector
does not trust the descriptor's self-asserted fingerprint: it hashes the exact
inclusive `0x8000..0x8bff` bytes and emits the runtime configuration mapping an
operator can independently review. Current reviewed source produces layout
`nivalo-provisioning-4mb-v1`; changing the partition CSV without updating and
reviewing the embedded fingerprint makes the post-build action fail closed.

## Release boundary

These tools do **not** sign, upload, publish, approve, or deploy an artifact.
The current `productionEligible:false` evaluation artifact must not be signed or
published as the commercial provisioning channel. Once an encrypted-storage
artifact exists and passes separate review, the guarded platform artifact
pipeline must sign its exact merged bytes server-side with its configured
firmware-signing private key. Only the corresponding SPKI public key belongs in
console runtime configuration. Never place a signing private key in this
repository, PlatformIO flags, the firmware, or generated evidence.
