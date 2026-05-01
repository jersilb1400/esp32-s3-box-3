#!/usr/bin/env zsh
# Source this file to put the shell into a clean ESP-IDF v5.5.2 state
# for building this project. Does NOT run idf.py — just prepares env.
#
# Usage (in a fresh or dirty shell):
#   source scripts/use-idf-5.5.sh
#   idf.py build
#   idf.py -p /dev/cu.usbmodem2101 flash monitor
#
# Why this script exists:
#   This Mac also has ESP-IDF v6.0 installed under ~/.espressif/v6.0/.
#   v6.0 removed the core `mqtt` component, which this project needs,
#   so builds MUST use v5.5.x. Sourcing v6.0's export.sh leaves behind
#   env vars (IDF_PATH, IDF_PYTHON_ENV_PATH, PATH entries) that will
#   hijack v5.5 even after you try to re-source v5.5's export.sh.
#   This helper wipes those first, then sources v5.5.2.

# Clear any state left by a previous IDF session (v6.0 or otherwise).
unset IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_EXPORT_CMD IDF_TOOLS_INSTALL_CMD \
      ESP_IDF_VERSION OPENOCD_SCRIPTS

export IDF_PATH="$HOME/esp-idf-v5.5.2"

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  echo "ERROR: $IDF_PATH/export.sh not found." >&2
  echo "Install v5.5.2 first — see docs/BUILD_NOTES.md." >&2
  return 1
fi

source "$IDF_PATH/export.sh"
echo
echo "ESP-IDF ready: $(idf.py --version 2>/dev/null | tail -1)"
echo "Project: $(pwd)"
