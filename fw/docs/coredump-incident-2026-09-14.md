# Coredump boot error — 2026-09-14

The ESP32-S3 on COM5 reported `Incorrect size of core dump image: -932099`.

## Confirmed cause

The coredump partition at `0x3F0000`, size `0x10000`, contained residual executable code from an earlier, different firmware. Its first bytes were `FD C6 F1 FF`: unsigned `0xFFF1C6FD`, printed as signed `-932099`. ESP-IDF interprets those bytes as a coredump length and rejects it because it exceeds 64 KiB.

Evidence from the original 8 MiB flash backup:

- SHA-256: `1e020134ea972a444cf6531323a0059c57d1009992457cfe902eec95d327ac86`.
- The current app occupies `0x10000..0x17459F`; normal upload erases only through `0x174FFF` for that app. It never initializes `0x3F0000`.
- All 16 sectors of the coredump partition contained nonblank data. Several code sequences match current firmware library code (up to 86 consecutive bytes).
- Residual data outside the current app includes IDF `v5.1.1-577-g6b1f40b9bf-dirty` at `0x30E6F8`, Linux `/root/.platformio/...` build paths and `ESP32_Display_Panel`. The current app uses IDF `v4.4.3`.
- The on-device partition table matches the project. The older 16 MiB project layout is historical context, not proof that it produced these particular residual bytes. The exact earlier firmware name and upload date cannot be recovered from the available evidence.

Ordinary uploads preserve sectors outside their image ranges, which explains why identical current firmware behaves differently on devices with different flash history. See [Espressif's erase-before-write behavior](https://docs.espressif.com/projects/esptool/en/release-v4/esp32/esptool/basic-commands.html#erasing-flash-before-write).

## Repair and prevention

The previous diagnostic session backed up the whole flash and cleared only the 64 KiB coredump partition. This session read it back and confirmed all bytes were `FF`.

`tools/coredump_upload.py` is registered as a PlatformIO upload pre-action. Normal serial firmware upload now reads and backs up the coredump partition first. If its size word is invalid, a blank image for exactly that partition is added to the same firmware upload command. Blank and valid-size records remain unchanged. Table validation and image-overlap checks precede this action; failed or incomplete reads abort the upload. Logging implementation and settings are unchanged.

Backups are under `.pio/coredump-backups/`; copy them elsewhere before deleting `.pio`. Original diagnostic evidence and boot captures are under `.pio/device-diagnostics/20260914-coredump/` and the original attachment-session diagnostic directory.

Scope: this prevents recurrence of the reported invalid-size error through this project's normal serial PlatformIO upload. Valid-size records are preserved for crash analysis; checksum errors and uploads using external tools require separate diagnosis.

## Verification

- Original bad header, size boundaries, blank/valid-size preservation, exact repair image, backup equality and short-read rejection passed host checks.
- Full build and normal upload to COM5 succeeded, including the new preflight and esptool hash verification.
- Two six-second serial boot captures showed ROM and application entry, without the reported coredump error, panic or repeated resets.
- The invalid-record repair image was checked offline against the original backup; the already repaired device exercised the preserve path during upload.

## Subsequent display investigation (still open)

The boot captures above establish only ROM/bootloader progress and absence of the coredump message; they do NOT prove that the thermostat UI is visible. The user reported a blank display afterward.

With explicitly approved PROJECT_LOG_LEVEL=3, build 252 reached `Setup complete`, initialized 8 MiB PSRAM and LittleFS, selected brightness 900, completed `gfx->begin()` and the first LVGL timer handler. PCA9554 at 0x20 did not acknowledge. That is a separate hardware/configuration fault; it has not been established as the cause of the blank display.

A bounded red/green/blue/white framebuffer test was added in `src/hal.cpp` for diagnosis. Its upload was interrupted at 84% when COM5 disappeared from Windows. The firmware must be uploaded again after reconnection. Display functionality remains unverified; do not treat a successful upload or `entry` line as proof of visible UI.
