# Build notes — ESP32-S3-BOX-3 (Xiaozhi fork)

Pinned guidance for this repo. Keep this file updated when build tooling changes.

## Required toolchain

- **ESP-IDF v5.5.2** (or newer 5.5.x). **Do NOT use v6.0.**
  - v6.0 removed/reworked the core `mqtt` component, so CMake fails with
    `unknown component mqtt`. Xiaozhi's `idf` dependency pins 5.5.x.
- **Target chip:** `esp32s3` (xtensa-esp32s3-elf toolchain).
- **Project path must contain no spaces.** Three managed components
  (`espressif__esp_audio_codec`, `espressif__esp_audio_effects`,
  `espressif__esp_image_effects`) emit unquoted `-L<dir>` linker flags,
  which break the final link if any path segment contains whitespace
  (e.g. `.../Grace Fellowship Info/...`). Canonical working tree on this
  machine: `~/esp-box3` (with a symlink at the old Documents location so
  existing links keep resolving).

## One-time ESP-IDF install (v5.5.2)

```bash
cd ~
git clone -b v5.5.2 --recursive --depth 1 https://github.com/espressif/esp-idf.git esp-idf-v5.5.2
cd esp-idf-v5.5.2
./install.sh esp32s3
```

## Every new terminal

Fastest and safest — use the helper shipped in this repo:

```bash
cd ~/esp-box3
source scripts/use-idf-5.5.sh
```

The helper clears any leftover ESP-IDF env vars (important on this Mac
because v6.0 is also installed) and then sources v5.5.2's `export.sh`.
It prints `ESP-IDF ready: ESP-IDF v5.5.2` on success.

Manual equivalent if you prefer:

```bash
unset IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_EXPORT_CMD IDF_TOOLS_INSTALL_CMD
export IDF_PATH="$HOME/esp-idf-v5.5.2"
source "$IDF_PATH/export.sh"
```

Verify:

```bash
idf.py --version            # should show v5.5.2
which idf.py                # should be under ~/esp-idf-v5.5.2/tools
```

> **Paste gotcha.** Do NOT paste the two lines `source "$IDF_PATH/export.sh"` and
> `cd ~/esp-box3` joined together — some terminals drop the newline and you end
> up trying to source a file literally named `export.shcd`. Run them as
> separate lines or just use the helper above.

## Clean build for this project

Always wipe `build/` when changing IDF version or target — stale CMake cache
is the usual cause of "wrong chip" / "wrong toolchain" symptoms.

```bash
cd ~/esp-box3                       # canonical no-space path
rm -rf build sdkconfig              # sdkconfig optional, forces re-merge of defaults
idf.py set-target esp32s3           # MUST run before first build after wipe
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Symptoms → cause cheat sheet

| Symptom in build log | Likely cause | Fix |
|---|---|---|
| `xtensa-esp32-elf-gcc` in commands | Target is `esp32`, not `esp32s3` | `rm -rf build && idf.py set-target esp32s3` |
| `CMake Error ... unknown component: mqtt` | ESP-IDF v6.0 sourced | `source scripts/use-idf-5.5.sh` |
| `Requirement '…' was not met. Installed version: …` + `IDF_PYTHON_ENV_PATH: .../idf6.0_py3.14_env` | v6.0 env vars leaking into a shell that then sourced v5.5.2 | `source scripts/use-idf-5.5.sh` (it `unset`s the stale vars first) |
| `.: no such file or directory: .../export.shcd` | Pasted newline was dropped between `export.sh` and `cd` | Run the lines separately, or use the helper |
| `ld: cannot find Fellowship: No such file or directory` (or any unknown `-L<word>`) during final link | Project path contains spaces; managed components emit unquoted `-L` flags | Build from a no-space path (`~/esp-box3`) |
| Assets missing / wake word not recognized | `sdkconfig` out of sync with defaults | Delete `sdkconfig`, rebuild — defaults merge automatically via `CMakeLists.txt` |
| `A fatal error occurred: File generated_assets.bin (length N) at offset 0x800000 will not fit in 16777216 bytes of flash` | Old stock `v2/16m.csv` has only 8 MB for assets; this preset builds ~10 MB | Preset now selects `partitions/v2/16m_box3_xiaozhi.csv` (12 MB assets, no OTA). If you forked the preset, match that selection. |

## Flash layout (BOX-3 preset, 16 MB)

This preset uses a **custom, single-app layout** —
`partitions/v2/16m_box3_xiaozhi.csv` — selected by
`sdkconfig.defaults.esp-box3-xiaozhi`:

```
0x000000  bootloader
0x008000  partition_table           (4 KB)
0x009000  nvs                       (16 KB, preserves WiFi creds across reflash)
0x00f000  phy_init                  (4 KB)
0x020000  factory  app              (4 MB   — ~16 % free with current build)
0x410000  assets   spiffs           (12 MB  — fits the ~10 MB Multinet7 + emoji + fonts bundle)
0x1000000 end of flash
```

Trade-off: **no OTA**. Firmware updates must be flashed over USB. If you
ever re-enable OTA, you need to either shrink `generated_assets.bin`
(disable custom wake word or drop emoji/fonts) or move to a 32 MB module.

## Why this project expects these defaults

See top-level `README.md` and `sdkconfig.defaults.esp-box3-xiaozhi`. The
BOX-3 preset enables: `CONFIG_BOARD_TYPE_ESP_BOX_3`,
`CONFIG_FLASH_DEFAULT_ASSETS`, English Multinet wake word `grace`
(display `Grace`), device-side AEC, the audio processor pipeline, and
the single-app 12 MB-assets partition table described above.

## Sensor support (BOX-3 + SENSOR/DOCK/BREAD/BRACKET)

The `esp-box-3` board implementation now includes sensor support using
Espressif components:

- `espressif/icm42670` (base IMU on BOX-3)
- `espressif/aht20` (temperature/humidity on SENSOR dock)
- `espressif/at581x` (radar on SENSOR dock)
- `espressif/i2c_bus` (dock I2C transport)

### Pin mapping used by firmware

From Espressif `esp-box` BSP/factory-demo sources:

- Dock I2C: `GPIO40` SCL, `GPIO41` SDA
- Radar OUT: `GPIO21`
- IR control: `GPIO44` (low enables TX path)
- IR TX: `GPIO39`
- IR RX: `GPIO38`
- IMU I2C (base unit): `GPIO18` SCL, `GPIO8` SDA

### MCP tools added

- `self.sensor.get_status`
- `self.sensor.get_environment`
- `self.sensor.get_imu`
- `self.sensor.get_radar_presence`
- `self.sensor.set_radar_enabled`
- `self.sensor.get_ir_rx_level`
- `self.sensor.set_ir_tx_enabled`
- `self.pmod.read_gpio`
- `self.pmod.write_gpio`

### PMOD mapping exposed (`self.pmod.*`)

- PMOD1: IO1=`GPIO42`, IO2=`GPIO20`, IO3=`GPIO39`, IO4=`GPIO40`,
  IO5=`GPIO21`, IO6=`GPIO19`, IO7=`GPIO38`, IO8=`GPIO41`
- PMOD2: IO1=`GPIO13`, IO2=`GPIO9`, IO3=`GPIO12`, IO4=`GPIO44`,
  IO5=`GPIO10`, IO6=`GPIO14`, IO7=`GPIO11`, IO8=`GPIO43`

### Known limits

- Radar currently uses AT581x digital presence + hold timer semantics.
  Distance/zones are not yet exposed.
- IR currently exposes low-level RX state and TX path enable.
  Full IR learning/playback flows are not implemented in this firmware.
