# Registry release runbook

This repository contains publication preparation only. Adding this workflow does
not publish a package, create a tag or release, create a repository or remote,
or change source visibility.

## Package identities

| Package | PlatformIO name | Source metadata version | Release tag |
|---|---|---|---|
| Device library | `NivaloDevice` | `0.2.0` | `v0.2.0` |
| Maintained DAP fork | `Nivalo Adafruit DAP` | `1.8.3-nivalo.1` | `dap-v1.8.3-nivalo.1` |

Tags are immutable release identities. Never move or reuse one. Version changes
must update both `library.json` and `library.properties`; DAP changes must also
update `fork-manifest.json`. The release validator rejects mismatches.

The device package intentionally does not declare DAP as an unconditional
dependency because standalone devices do not compile the bridge. Registry users
building a bridge must install exact matching releases of both packages. Publish
and validate the DAP package before publishing a device release that documents
it as the bridge dependency.

## GitHub configuration

Create a protected GitHub Actions environment named `registry-publication` with
required reviewers. Configure these repository or environment secrets:

- `NIVALO_DEVICE_PLATFORMIO_AUTH_TOKEN`
- `NIVALO_DAP_PLATFORMIO_AUTH_TOKEN`

Use separate, least-privilege PlatformIO tokens so either package can be revoked
without granting access to the other. Configure these non-secret Actions
variables with the approved PlatformIO account or organization owner names:

- `NIVALO_DEVICE_PLATFORMIO_OWNER`
- `NIVALO_DAP_PLATFORMIO_OWNER`

Do not add Arduino, GitHub, or PlatformIO credentials to source, workflow inputs,
release archives, or logs. The workflow maps the selected named token to
`PLATFORMIO_AUTH_TOKEN` only inside the publish step.

## Dry run

Before creating a tag, run locally:

```sh
python tests/validate_registry_release.py
python scripts/prepare_registry_release.py --package device --tag v0.2.0 --dap-owner <approved-owner> --output .release-stage
python scripts/prepare_registry_release.py --package dap --tag dap-v1.8.3-nivalo.1 --output .release-stage
python scripts/pack_registry_archive.py .release-stage/device --output dist/device-0.2.0.tar.gz
python scripts/pack_registry_archive.py .release-stage/dap --output dist/dap-1.8.3-nivalo.1.tar.gz
python scripts/verify_registry_archives.py \
  --device dist/device-0.2.0.tar.gz \
  --dap dist/dap-1.8.3-nivalo.1.tar.gz \
  --fresh-core-dir <empty-platformio-core>
```

Use `dap` and `dap-v1.8.3-nivalo.1` for the fork. Inspect the archive file list
and SHA-256. The staging script excludes tests, repository metadata, build
outputs, the vendored DAP tree from the device archive, ignored developer
configuration, symlinks, and private-key PEM material.

The staged device package retains the browser-provisioning post-build script and
its artifact helper at the exact path used by the packaged example. Its bridge
example is normalized for a registry consumer: workstation upload/monitor ports
and repository-local DAP paths are removed, and the exact reviewed
`<approved-owner>/Nivalo Adafruit DAP@1.8.3-nivalo.1` dependency is selected.
The owner comes from the reviewed `NIVALO_DAP_PLATFORMIO_OWNER` variable, so a
same-name package under a different namespace cannot satisfy the release. The
source examples remain optimized for repository-local development. Release
validation fails closed if any of these packaged-tree invariants drift.

CI packs both release candidates and builds three clean consumers from those
exact archives: the browser-provisioning artifact, a standalone device, and a
bridge with the maintained DAP package. The release workflow repeats the same
archive builds before it retains the selected package. It also runs the official
Arduino Lint 1.3.0 binary after verifying its published SHA-256; normal CI uses
specification compliance. Strict Library Manager submission compliance runs for
a device release only after the repository has deliberately been made public,
because the official submission check must be able to resolve the repository
URL. PlatformIO validation and publication do not require that visibility
change.

The `Registry release` workflow can also be dispatched with `publish=false` and
an existing `refs/tags/...` ref. It reruns firmware validators, both repository
builds, staging, packing, archive inspection, and checksum generation without
contacting a registry.

The repository packer writes a byte-reproducible archive: members are sorted,
ownership and modes are canonical, and tar/gzip timestamps are zero. Repeating
the stage and pack steps for the same source must produce the same SHA-256 with
the pinned release toolchain, independent of source file timestamps, line-ending
style, and workstation permission bits. Repository text is checked out as LF,
and the packer also canonicalizes the explicit release-text formats before
archiving. It refuses to overwrite an archive, so use a new
output path for a second comparison. Release CI builds the exact archives with an explicitly
empty `PLATFORMIO_CORE_DIR`; this proves that success does not depend on a
developer's cached platform, framework, toolchain, or library packages. The
verifier installs the exact pinned platform into that empty directory before
compilation, then installs all library dependencies while building the three
fresh consumers. Archive-consumer builds use one compiler job so a first-use
toolchain extraction is not coupled to workstation CPU/process limits. On
Windows it also compiles a throwaway object with the newly extracted toolchain,
using a bounded three-attempt readiness probe before any package build.
Keep the Windows core path short (for example `C:\pio-empty`) because the pinned
Xtensa GCC toolchain is not long-path safe. The verifier fails without falling
back to the normal PlatformIO cache.

The prepare job retains its exact archive for seven days as a workflow artifact.
The protected publish job checks out the reviewed commit SHA, proves the tag
still identifies it, downloads that same archive, and verifies its recorded
SHA-256 instead of rebuilding a potentially different tarball.

## PlatformIO publication

1. Confirm the repository visibility and package owner are approved. The
   workflow makes no assumption that either is public.
2. Audit the exact commit, dependency graph, upstream DAP provenance and license.
3. Create a signed, immutable tag outside this workflow only after approval.
4. Prefer a manual dry run against that tag.
5. Publish DAP first. Confirm it resolves under the configured owner and build a
   bridge using its exact package version.
6. Publish the device package. Confirm a fresh standalone install and a bridge
   install that explicitly includes the DAP package.
7. Record tag, commit, archive SHA-256, registry owner/package/version, workflow
   run, approver and verification results in the release evidence.

A matching tag push is validation-only. Publication requires a manual dispatch
against the exact immutable `refs/tags/...` ref with `publish=true`, the exact
confirmation `PUBLISH <package> <tag>`, and approval in the protected
`registry-publication` environment. Device publication also verifies that the
exact reviewed DAP version already resolves under its configured PlatformIO
owner. The workflow never creates GitHub releases.

## Arduino Library Manager readiness

`NivaloDevice` now has root metadata, an MIT license file, examples, and a
SemVer-compatible `v0.2.0` tag convention. Arduino submission still requires
an explicitly approved public GitHub repository, an immutable release tag, a
release validation run, and a deliberate submission to the official Arduino
Library Registry. Do not submit a private or unreviewed repository.

The DAP fork is nested and therefore is **not** ready for Arduino Library Manager
submission from this repository. First create an approved dedicated public
repository such as `Nivalo_Adafruit_DAP`, preserving upstream commit and license
provenance; put the current `source/` contents at its root; retain
`upstream-provenance.json` and `fork-manifest.json`; validate root
`library.properties`; then create an immutable `1.8.3-nivalo.1` release tag and
submit that repository separately. This runbook does not authorize or perform
those operations.

After either Arduino submission, verify index ingestion and install/build from a
fresh Arduino Library Manager cache. Record the registry index version and test
result. PlatformIO publication does not prove Arduino readiness.

## Rollback and deprecation

Published versions are immutable. Never overwrite a package version or move its
tag. If validation fails before publication, reject the environment approval,
delete only local staging artifacts, and fix the source under a new commit.

If a bad version is published:

1. Disable the `registry-publication` environment and revoke the affected named
   PlatformIO token.
2. Mark the version deprecated or remove it only through an owner-reviewed
   registry operation supported by the registry; do not assume deletion is
   reversible.
3. Publish a corrected patch version under a new tag and document the affected
   version, impact and upgrade path.
4. Keep provenance and release evidence for the withdrawn version.

For DAP incompatibility, deprecate the DAP version and every device release that
recommends it together. Keep at least the prior known-good pair documented until
the replacement has passed both bridge builds and attended STM32 OTA testing.
