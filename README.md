# Xiaozhi firmware — ESP-BOX-3 preset fork

This repository is a **standalone copy** of [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) with **saved defaults** for the **Espressif ESP-BOX-3** kit: default chat UI, **built-in assets** (so speech models match `sdkconfig`), **English Multinet** wake phrase **`grace`** (shown as **Grace** to the server), audio processor, and **device-side AEC**.

Upstream application logic, board ports, and licenses are unchanged unless noted in git history.

## Requirements

- **ESP-IDF v5.5.2** (or newer 5.5.x matching the project’s `idf` component constraint). **Do NOT use ESP-IDF v6.0** — core `mqtt` was removed/reworked and CMake will fail with `unknown component mqtt`. Install per [Espressif docs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html).
- Target chip must be `esp32s3`. Always run `idf.py set-target esp32s3` on a clean tree before `idf.py build`, otherwise the default `esp32` target (xtensa-esp32-elf toolchain) will be used.
- On Debian/Ubuntu you may need: `sudo apt install python3.12-venv` (or matching `python3-venv` for your Python).

See [`docs/BUILD_NOTES.md`](docs/BUILD_NOTES.md) for the full recovery/clean-build procedure and a symptoms-to-cause table.

## Preset configuration

File `sdkconfig.defaults.esp-box3-xiaozhi` is merged **after** `sdkconfig.defaults` and `sdkconfig.defaults.esp32s3` via `CMakeLists.txt`, so a clean tree gets:

| Option | Purpose |
|--------|---------|
| `CONFIG_BOARD_TYPE_ESP_BOX_3` | BOX-3 pinout and drivers |
| `CONFIG_USE_DEFAULT_MESSAGE_STYLE` | Standard chat UI (fits default generated assets) |
| `CONFIG_FLASH_DEFAULT_ASSETS` | Build `generated_assets.bin` from `sdkconfig` (includes `index.json` + `srmodels.bin`) |
| `CONFIG_SR_MN_EN_MULTINET7_QUANT` | English Multinet model (required for English wake phrase) |
| `CONFIG_USE_CUSTOM_WAKE_WORD` + `CONFIG_CUSTOM_WAKE_WORD` / `_DISPLAY` | Phrase **`grace`**, display **Grace** (train users to say “grace” clearly; tweak threshold in menuconfig if needed) |
| `CONFIG_USE_AUDIO_PROCESSOR` | Noise reduction pipeline |
| `CONFIG_USE_DEVICE_AEC` | Device-side echo cancellation |

Use `idf.py menuconfig` to change the phrase, threshold, or switch back to **AFE / fixed Wakenet** (and optional Espressif BOX-3 custom asset URL) if you prefer the stock “你好小智” style wake word.

## Build and flash

```bash
export IDF_PATH=/path/to/esp-idf   # v5.5.2+
. $IDF_PATH/export.sh
cd esp32-s3-box-3
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
```

The build produces **`build/generated_assets.bin`** (fonts, emojis, `srmodels.bin`, etc.) and flashes it to the **assets** partition together with the app.

Hardware-specific notes for BOX-3 and add-ons are in `main/boards/esp-box-3/README.md`.

## Git remotes

Canonical fork URL: [github.com/jersilb1400/esp32-s3-box-3](https://github.com/jersilb1400/esp32-s3-box-3).

Clone:

```bash
git clone https://github.com/jersilb1400/esp32-s3-box-3.git
cd esp32-s3-box-3
```

If you already have a local copy and need to publish:

```bash
git remote add origin https://github.com/jersilb1400/esp32-s3-box-3.git
git push -u origin main
```

To pull fixes from upstream [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32):

```bash
git remote add upstream https://github.com/78/xiaozhi-esp32.git   # if not already added
git fetch upstream && git merge upstream/main
```
