# Nivalo maintained Adafruit DAP fork

This directory contains Nivalo's active repository-local Adafruit DAP fork.
`source/` is an import of official tag `1.8.3` at commit
`8ca356d92e73d0d1005534030849e7ca37324805`, including the upstream BSD license.
`upstream-provenance.json` records normalized upstream hashes and the two files
intentionally changed by the fork.

The maintained delta is deliberately small:

1. `Adafruit_DAP_STM32.cpp` adds STM32 device ID `0x421`.
2. `library.properties` assigns the local fork identity/version and exact
   external dependency constraints.

Both bridge projects resolve this source through the symlink paths in
`fork-manifest.json`. The Nivalo library metadata omits a registry dependency
until this fork is published; bridge consumers must supply the fork explicitly.
No remote or package has been created.

For an upstream refresh, import the new reviewed tag, update provenance hashes,
reapply or retire the delta, and pass drift validation plus both bridge builds.

Bridge `platformio.ini` files repeat the exact DAP-chain versions, including
TinyUSB's MIDI dependency. `SD` is supplied locally by the pinned Espressif32
platform rather than resolved as a floating registry package.
