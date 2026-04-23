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

## Why this project expects these defaults

See top-level `README.md` and `sdkconfig.defaults.esp-box3-xiaozhi`. The
BOX-3 preset enables: `CONFIG_BOARD_TYPE_ESP_BOX_3`,
`CONFIG_FLASH_DEFAULT_ASSETS`, English Multinet wake word `grace`
(display `Grace`), device-side AEC, and the audio processor pipeline.
