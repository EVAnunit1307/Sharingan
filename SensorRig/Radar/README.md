# Radar — HLK-LD2450 mmWave presence/tracking

`ld2450_radar.py` runs on the Pi. It's the classic "radar tracks you through
a wall" demo, served as a web page instead of needing a dedicated TFT +
ESP32 — great for a two-person through-wall test where whoever has the
browser open watches the dot move on their phone.

## Hardware

- **Sensor:** HLK-LD2450, wired to the Pi's UART (`/dev/serial0`) at
  **256000 baud**.
- **Spec:** ~120° field of view, up to 6m forward range, ±3m lateral, up to
  3 simultaneous tracked targets.
- **Permissions:** reading `/dev/serial0` without `sudo` requires the Pi
  user to be in the `dialout` group:

  ```
  sudo usermod -aG dialout <your-pi-username>
  ```

  then log out and back in — group membership only takes effect on a fresh
  login.

## Wire protocol

Frames are fixed-length (30 bytes), delimited by a 4-byte header
(`AA FF 03 00`) and 2-byte footer (`55 CC`), carrying up to 3 targets'
signed X/Y position (mm) and speed (cm/s) as little-endian `HHHH` tuples per
target. `parse_frame()` is the reference implementation for the sign
convention (top bit = sign, remaining 15 bits = magnitude) if you're
decoding this sensor elsewhere.

## Running it

```
python3 ld2450_radar.py
```

Then open `http://<pi-hostname-or-ip>:8767` from any phone/laptop on the
same network — this works over plain local WiFi and doesn't need the PC
ground station running at all.

| Endpoint | Purpose |
|---|---|
| `/` | Live radar-scope web page |
| `/targets` | Raw JSON: current tracked targets + sensor connection status |

## Notes

- `Fusion/wallhack_dashboard.py` reuses this exact framing/parsing logic, so
  a protocol fix here should be mirrored there (or better, refactor both to
  share one module if you're touching this).
- The reader loop reconnects automatically if the serial port drops —
  `sensor_ok` in `/targets` tells you whether it's currently getting valid
  frames.
