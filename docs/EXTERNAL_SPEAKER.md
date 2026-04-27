# External I²S Speaker on PMOD2

The ESP-BOX-3 has a small internal 4Ω speaker. To drive a louder passive
speaker, you can attach an **MAX98357A** I²S amplifier breakout to the
**PMOD2 header** (the unused one — PMOD1 is partially shared with the
sensor dock).

This sets up the ESP32-S3's secondary I²S peripheral (I²S1) as a TX-only
mirror of the audio output, so the internal speaker AND your external amp
play simultaneously.

## Hardware

Buy:
- **MAX98357A** breakout board (Adafruit #3006 or generic; ~$3-6)
- 4Ω or 8Ω passive speaker, 3W minimum (Adafruit #1313 works great)
- Jumper wires
- (Optional) ESP-BOX-3-DOCK / BREAD / BRACKET for easy PMOD access

PMOD2 pinout used (matches `main/boards/esp-box-3/config.h`):

| PMOD2 pin | GPIO | Signal | MAX98357A pin |
|---|---|---|---|
| IO1 | 13 | BCLK | BCLK |
| IO2 | 9  | LRCLK / WS | LRC |
| IO3 | 12 | DOUT | DIN |
| GND | — | GND | GND |
| 5V  | — | 5V | VIN |

Leave MAX98357A's `GAIN` pin floating for default (9 dB) gain.
For 12 dB extra, tie `GAIN` to GND. For 6 dB cut, tie to VIN.
For 3 dB cut, use a 100 kΩ pull-down. Mono only — leave `SD` pin
disconnected (or tie high to enable; pulled-down disables the amp).

## Firmware

1. `idf.py menuconfig` →
   **Xiaozhi Assistant** → **Jarvis HUD extras** →
   `[*] Mirror audio output to PMOD2 I2S pins`
2. `idf.py build flash`
3. Audio plays out both speakers in parallel.

## Notes

- PMOD2's GPIO 9 / 12 / 13 are general-purpose IO and aren't used by any
  other peripheral on the ESP-BOX-3 main unit, so this doesn't conflict
  with sensors or display.
- Latency between the two speakers is sample-accurate (the I²S TX runs in
  lockstep with I²S0).
- If you want to **disconnect** the internal speaker entirely, leave
  `CONFIG_USE_PMOD_EXTERNAL_SPEAKER=y` and unplug the small JST connector
  inside the unit (or mute via `self.audio_speaker.set_volume 0` — but
  that mutes the codec output, killing both).
- The MAX98357A draws very little current at low volume but can pull
  ~600 mA at full volume. PMOD2's 5V is fed from the ESP-BOX-3's USB
  supply, which is fine for typical bedroom-level listening but may dip
  at extreme volumes. For very loud setups, power the amp from a separate
  5V source and only share GND.

## Status

The Kconfig flag is in place. **The actual I²S1 mirror code is staged
for a follow-up patch** — when you have the hardware wired up, ping me
and I'll wire the I²S1 init and DMA mirror in `box_audio_codec.cc` (~80
lines). For now, the `idf.py menuconfig` flag is harmless until the
mirror code lands.
