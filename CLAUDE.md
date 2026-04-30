# ESP32-S3 Box-3 (JARVIS firmware)

## What this repo is

- **ESP-IDF v5.5.x** C++ firmware for **Espressif ESP-BOX-3** running the Xiaozhi / voice assistant stack.
- **Wake phrase:** configurable in `sdkconfig.defaults.esp-box3-xiaozhi` — production Jarvis builds use **`jarvis`** (Multinet EN); docs may mention older **`grace`** preset — verify your tree.
- **Board:** `esp32s3`; use `sdkconfig.defaults.esp-box3-xiaozhi` merged via CMake preset.

## Companion backend

- Python server (**xiaozhi-esp32-server** fork) lives in **`~/jarvis-server`** — **separate repo** (private), deployed to Fly app **`jarvis-server`** at `https://jarvis-server.fly.dev/`.
- **OTA URL:** `https://jarvis-server.fly.dev/xiaozhi/ota/` (confirm in server config if OTA routing changes).

## Build

```bash
. $IDF_PATH/export.sh
cd "$(git rev-parse --show-toplevel)"
idf.py set-target esp32s3
idf.py build -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp-box3-xiaozhi"
```

**Do not use ESP-IDF v6.x** — project pins 5.5.x. Project path **must not contain spaces** (linker flags).

## Planning / enhancement work

- Active phased plans live under **`docs/plans/`** (repo consolidation, speaker ID, memory, integrations).

## Git

- **Default branch:** `main`; push CI builds via `.github/workflows/build.yml`.
- **Remote:** `origin` → `jersilb1400/esp32-s3-box-3`.
