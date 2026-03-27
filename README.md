# ESP32 Beverage Antenna Controller

## Summary

This project implements a two-station antenna controller for a beverage antenna system.

- **CLUB station** (`src/CLUB_STATION.cpp`) is the authoritative controller. It manages physical antenna switching, drives the DTMF encoder and relay outputs, and publishes the confirmed state over MQTT.
- **REMOTE station** (`src/REMOTE_STATION.cpp`) is a UI client. It sends selection requests to CLUB via MQTT and renders the CLUB-confirmed state on its own TFT display.

Remotes do not become the source of truth — they only request. CLUB decides and reports the real result. This keeps behaviour deterministic: local and remote actions both flow into the same controller.

## Building

Only one station file can be active at a time (both define `setup()` and `loop()`). To select which station to build, rename the inactive file with a trailing underscore to exclude it from compilation:

```
# Build CLUB:
src/CLUB_STATION.cpp       ← active
src/REMOTE_STATION.cpp_    ← disabled

# Build REMOTE:
src/CLUB_STATION.cpp_      ← disabled
src/REMOTE_STATION.cpp     ← active
```

Select the target hardware environment in `platformio.ini`:
```ini
default_envs = daniel   ; or henryk
```

## Notes

- **WiFi connection and callsign acquisition:** on boot, the firmware tries to load saved WiFi credentials from NVS through HB9IIUPortal. If valid credentials are stored, it connects directly. Otherwise it starts a captive portal where the user enters WiFi credentials and callsign, which are then saved for subsequent boots.

- **Factory reset:** holding the encoder push button during boot performs a full NVS erase and restarts, clearing all stored credentials, callsign, presets, and configuration.

- **Antenna selection workflow (CLUB):** encoder rotation enters a preview phase on the dial. The actual antenna change is only committed after rotation stops for `PREVIEW_SETTLE_MS`. At commit time, the firmware executes the physical switch and publishes the new state over MQTT.

- **Antenna selection workflow (REMOTE):** encoder rotation updates the local preview needle immediately. After `PREVIEW_SETTLE_MS` of inactivity, a SELECT command is published to CLUB via MQTT. The needle stays at preview position until CLUB confirms. Commit is blocked if CLUB is offline.

- **MQTT topics:**

  | Topic | Direction | Description |
  |---|---|---|
  | `antenna/state` | CLUB → REMOTE | Authoritative retained state |
  | `antenna/cmd` | REMOTE → CLUB | Selection requests |
  | `antenna/heartbeat` | CLUB → REMOTE | Presence beacon, every 5 s, non-retained |

- **REMOTE MQTT LED indicator** (bottom-right corner of TFT):

  | Colour | Pattern | Meaning |
  |---|---|---|
  | Red | Solid | No WiFi |
  | Red | Blinking | WiFi ok, MQTT broker unreachable |
  | Orange | Solid | MQTT ok, CLUB offline (no heartbeat for 15 s) |
  | Green | Solid | MQTT ok, CLUB alive, idle |
  | Yellow | Blinking | Awaiting CLUB confirmation |

- **Troubleshooting — racing commits:** if fast rotation causes multiple SELECT commands and relay/DTMF chatter, increase `PREVIEW_SETTLE_MS` in `REMOTE_STATION.cpp` (default 500 ms). This forces the user to pause longer before a commit is sent.

- **User interface:** three views are available — antenna dial (primary control), great-circle map (visualises azimuth from home QTH), and a keypad/relay page (drives PCF8574 outputs). Views are toggled by touching the screen.

- **Local control modes and presets:** the left and right hardware buttons operate in two modes. Mode A steps the antenna CW/CCW. Mode B recalls presets, with long-press on the encoder button to arm a preset save.

- **Hardware switching (CLUB only):** the committed antenna number is translated into a DTMF symbol sent via the HT9200A, which controls the downstream switching hardware. The SDR/TRX relay is driven directly from a GPIO pin. Additional relay outputs are driven via a PCF8574 I2C expander.

## Needle Background Restore — How It Works

The needle is drawn on top of `greatcircleMap.png`. Every time the needle moves, the pixels it covered must be restored before drawing it at the new position.

**Why not `tft.readRect()`?** The ILI9488 over SPI does not support pixel readback reliably — it returns garbage. Never use it.

**Previous approach (RAM cache, removed):** At startup, the 267×267-pixel region of the map centred on the needle pivot (cx=240, cy=160) was decoded from the PNG and kept as a 68 KB heap buffer. This left no room for the SSL/TLS heap needed by MQTT over port 8883, limiting `NEEDLE_L` to 90.

**Current approach (LittleFS raw file):** The same 267×267 region is pre-converted to a raw big-endian RGB565 binary file (`data/dialBg.raw`, 142 KB) using a Python script. At runtime the file stays open and needle restores are done by seeking to the correct row and reading only the dirty pixels into a 534-byte static buffer. This uses zero heap, leaving SSL free to operate, and allows `NEEDLE_L=130`.

**Regenerating `dialBg.raw`** is required if you change the needle length, the pivot point, or the background image:

```bash
python3 tools/gen_dialBg_raw.py   # rewrites data/dialBg.raw
pio run --target uploadfs -e daniel
pio run --target upload -e daniel
```

The constants `NEEDLE_L`, `CACHE_R`, `CACHE_W`, `CX`, and `CY` must be identical in both `tools/gen_dialBg_raw.py` and `src/needleStuff.h`.

**Flashing reminder:** `data/` (LittleFS) and the firmware are separate flash partitions. Any change to `data/` requires `uploadfs` in addition to `upload`.

## MQTT Explorer

Download MQTT Explorer from https://mqtt-explorer.com/ and create a connection to the same broker used by the firmware.

Connect with these settings:

- Open MQTT Explorer
- Create New Connection
- Set Host to 1db5ec5c4ff44e67bcb25a811f852e53.s1.eu.hivemq.cloud
- Set Port to 8883
- Turn on TLS/SSL
- Set Username to esp32-club
- Set Password to @V51bP9J@H
- Set Client ID to something unique, for example daniel-mqtt-test
- Click Connect