# Architecture

## Two machines, three wire protocols

| Link | Protocol | Carries |
|---|---|---|
| Flight controller ↔ PC | MSP (MultiWii Serial Protocol) over USB serial, 115200 baud | Attitude, raw IMU, motor readback/override |
| LD2450 radar ↔ Pi | UART, 256000 baud, fixed 30-byte framed packets | Up to 3 tracked targets' X/Y (mm) + speed (cm/s) |
| Pi ↔ PC | WiFi — HTTP (MJPEG + JSON) and WebSocket | Camera stream, radar JSON, fused pose/detections |

## Port map

| Port | Host | What's listening |
|---|---|---|
| `COM4` (or platform equivalent) | PC | Flight controller, MSP |
| `/dev/serial0` | Pi | LD2450 radar, raw UART frames |
| `8765` | Pi | WebSocket bridge (rig pose out, Pi detections back) — used by `Fusion/imu_viz.py` |
| `8766` | Pi | Camera MJPEG stream — either `CV/pi_camera_stream.py` alone, or `Fusion/wallhack_dashboard.py`'s combined dashboard |
| `8767` | Pi | Standalone radar web page — `Radar/ld2450_radar.py` only |

Note `8766` is shared between the two Pi-side "modes" described in the top
level README — `CV/pi_camera_stream.py` and `Fusion/wallhack_dashboard.py`
both use it, but you only ever run one of them at a time.

## Two ways to run the Pi side

**Split mode:** `CV/pi_camera_stream.py` + `Radar/ld2450_radar.py` as two
independent processes. Simplest to reason about; camera and radar know
nothing about each other.

**Fused mode:** `Fusion/wallhack_dashboard.py` alone, owning both the
camera and the radar serial port, running a better detector (YOLOv8n
instead of MobileNet-SSD/HOG), and cross-checking the camera's person-count
against the radar's target-count on one page.

Pick one. They can't run side by side — both modes want exclusive access to
the camera and to `/dev/serial0`.

## Position estimation: two different techniques, don't confuse them

- **Camera → person position** (`Fusion/wallhack_dashboard.py`,
  `estimate_person_position()`): monocular, single-frame, geometry-only.
  Bearing from box position in frame, distance from assumed person height
  vs. box height. No history, no filtering across frames — a rough overlay,
  recomputed fresh every frame.
- **IMU → rig position** (`Fusion/imu_viz.py`): gravity-compensated
  acceleration integrated over time (velocity, then position), with
  gyro-gated zero-velocity updates (ZUPT) to fight drift when the rig is
  actually still. This one *does* accumulate error over time like any
  dead-reckoning system — call `/reset` on a level, stationary bench
  whenever the drift gets visible, don't expect it to hold accuracy over a
  long session unmoored from a reset.

Neither of these is a Kalman filter. If you're looking to properly fuse the
radar's real range measurements with the camera's rough monocular estimate
and the IMU's dead-reckoning into one consistent state estimate (instead of
just plotting all three on the same scope and eyeballing agreement), that's
the natural next step here and doesn't exist yet.

## Detector ladder (camera side)

Both `CV/pi_camera_stream.py` and `Fusion/wallhack_dashboard.py` degrade
gracefully if a model file is missing, so cloning this repo onto a fresh Pi
"just works" at reduced accuracy rather than crashing:

```
YOLOv8n (Fusion only, best)  →  MobileNet-SSD (CV, if model files present)  →  HOG (CV fallback, always available)
```

`models/` is not checked into this repo (see `.gitignore`) — download the
weights per `Docs/setup.md` before running either script for the best
result.
