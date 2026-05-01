Jarvis HUD — consolidated data and tools
========================================

All Box-3 “Jarvis HUD” asset generators and their generated C sources for this
fork live under this folder so nothing is split between scripts/ and main/display/.

Layout
------
  tools/           Python generators (run manually or via CMake for the face blob)
  generated/       Build outputs (ignored by git except .gitignore); created by tools or idf.py build
  mesh_preview/    Vertex-only OBJ previews from the head-mesh tool (optional)
  samples/         Optional artist drop-ins (e.g. jarvis_face.png) — not required to build

Firmware code that consumes the face bitmap remains in main/display/ (e.g.
jarvis_artist_hud.cc). CMake points the main component at:
  jarvis_hud/generated/jarvis_artist_face_data.c

Typical commands (from repo root, no spaces in path)
----------------------------------------------------
  python3 jarvis_hud/tools/gen_jarvis_artist_face_asset.py   # RGB565 face C blob
  python3 jarvis_hud/tools/gen_jarvis_head_mesh.py          # optional .inc / OBJ for point-splat dev

The face generator runs at CMake configure time (root CMakeLists.txt), not via add_custom_command
in main/: ESP-IDF’s component-requirements pass cannot evaluate custom commands.

Editing the generator script triggers reconfigure (CMAKE_CONFIGURE_DEPENDS). To refresh the blob
without a full reconfigure you can still run python3 manually, then rebuild.
