# Nivalo ESP32

ESP32 Arduino/PlatformIO library for Nivalo devices.

`NivaloDevice` is the primary API for both standalone ESP32 devices and ESP32 devices that bridge to a secondary MCU. Secondary MCU support is enabled at build time with `NIVALO_HAS_SECONDARY_MCU=1`.

## Layout

```text
src/
  NivaloDevice.h
  NivaloDevice.cpp
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

## Runtime Telemetry

Call `device.publishRuntimeTelemetry()` from the sketch loop to publish the portal's standard runtime vitals:

- `uptime_ms`
- `wifi_rssi`
- `heap_used`
- `heap_free`
- `heap_total`

Sketches should prefer this helper over hand-written telemetry names for common ESP32 health metrics.
