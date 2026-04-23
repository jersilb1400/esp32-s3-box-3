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

```bash
export IDF_PATH="$HOME/esp-idf-v5.5.2"
. "$IDF_PATH/export.sh"
```

Verify:

```bash
idf.py --version            # should show v5.5.2
echo "$IDF_TARGET"          # optional; set-target writes sdkconfig
```

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
| `CMake Error ... unknown component: mqtt` | ESP-IDF v6.0 sourced | Source v5.5.2 (`. $IDF_PATH/export.sh`) |
| `ld: cannot find Fellowship: No such file or directory` (or any unknown `-L<word>`) during final link | Project path contains spaces; managed components emit unquoted `-L` flags | Build from a no-space path (`~/esp-box3`) |
| Assets missing / wake word not recognized | `sdkconfig` out of sync with defaults | Delete `sdkconfig`, rebuild — defaults merge automatically via `CMakeLists.txt` |

## Why this project expects these defaults

See top-level `README.md` and `sdkconfig.defaults.esp-box3-xiaozhi`. The
BOX-3 preset enables: `CONFIG_BOARD_TYPE_ESP_BOX_3`,
`CONFIG_FLASH_DEFAULT_ASSETS`, English Multinet wake word `grace`
(display `Grace`), device-side AEC, and the audio processor pipeline.
