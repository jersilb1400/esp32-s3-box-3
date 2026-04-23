# Xiaozhi firmware — ESP-BOX-3 preset fork

This repository is a **standalone copy** of [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) with **saved defaults** for the **Espressif ESP-BOX-3** kit: board type, emote UI, official BOX-3 custom assets bundle, AFE wake word, audio processor, and **device-side AEC** (double-tap Boot to toggle when enabled in UI).

Upstream application logic, board ports, and licenses are unchanged unless noted in git history.

## Requirements

- **ESP-IDF v5.5.2** (or newer 5.5.x matching the project’s `idf` component constraint). Install per [Espressif docs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html).
- On Debian/Ubuntu you may need: `sudo apt install python3.12-venv` (or matching `python3-venv` for your Python).

## Preset configuration

File `sdkconfig.defaults.esp-box3-xiaozhi` is merged **after** `sdkconfig.defaults` and `sdkconfig.defaults.esp32s3` via `CMakeLists.txt`, so a clean tree gets:

| Option | Purpose |
|--------|---------|
| `CONFIG_BOARD_TYPE_ESP_BOX_3` | BOX-3 pinout and drivers |
| `CONFIG_USE_EMOTE_MESSAGE_STYLE` | Emote / expression display path |
| `CONFIG_FLASH_CUSTOM_ASSETS` + `CONFIG_CUSTOM_ASSETS_FILE` | Official Espressif BOX-3 assets URL (downloaded at configure/build time) |
| `CONFIG_USE_AFE_WAKE_WORD` | Wake word with AFE (fits BOX-3 + PSRAM) |
| `CONFIG_USE_AUDIO_PROCESSOR` | Noise reduction pipeline |
| `CONFIG_USE_DEVICE_AEC` | Device-side echo cancellation |

Use `idf.py menuconfig` to turn options on or off (e.g. default chat UI instead of emote, or hotspot vs BluFi provisioning).

## Build and flash

```bash
export IDF_PATH=/path/to/esp-idf   # v5.5.2+
. $IDF_PATH/export.sh
cd esp-box3-xiaozhi
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
```

The first CMake run may **download** the custom assets `.bin` into `build/` for flashing to the assets partition.

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
