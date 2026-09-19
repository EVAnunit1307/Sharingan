# SensorRig

The physical sensing + fusion rig that feeds real-world tracking data into
Sharingan — a Raspberry Pi carrying a camera and an LD2450 mmWave radar,
paired with a PC that reads a flight-controller IMU and stitches everything
into one live picture.

This folder is software only (Python, runs on the Pi and on a PC). It is
kept separate from `Source/`, `Content/`, etc., which are the Unreal project
itself.

## Layout

| Folder | Runs on | What it does |
|---|---|---|
| [`Firmware/`](Firmware) | PC | Talks MSP directly to the flight controller for bench motor testing |
| [`CV/`](CV) | Pi | Camera capture + person/object detection, streamed as MJPEG |
| [`Radar/`](Radar) | Pi | LD2450 mmWave radar reader, served as a live web page |
| [`Fusion/`](Fusion) | Pi + PC | Combines camera, radar, and IMU into one dashboard |
| [`Docs/`](Docs) | — | Setup steps and the deeper architecture notes |

Each subfolder has its own README with what the script does, how to run it,
and any gotchas. Start with [`Docs/setup.md`](Docs/setup.md) if you're
setting this up for the first time.

## Architecture at a glance

```
 ┌─────────────────────── Raspberry Pi 5 (larp-pi.local) ───────────────────────┐
 │                                                                               │
 │   CSI Camera ──▶ CV/pi_camera_stream.py ──▶ MJPEG :8766/stream               │
 │                        (or Fusion/wallhack_dashboard.py, which owns          │
 │   LD2450 Radar ──▶      both the camera AND the radar serial port at        │
 │   (/dev/serial0) ──▶     once and serves a single fused dashboard on :8766) │
 │                        └──▶ Radar/ld2450_radar.py ──▶ JSON/web :8767        │
 │                                                                               │
 │   Fusion/wallhack_dashboard.py also runs a websocket bridge on :8765,       │
 │   exchanging rig-pose + person-detections with the PC side.                 │
 └───────────────────────────────────┬───────────────────────────────────────┘
                                      │  WiFi (home network "TS565", or a
                                      │  phone hotspot cloned to match it)
                                      ▼
 ┌─────────────────────────────── PC (ground station) ───────────────────────────┐
 │                                                                                │
 │   Flight controller ──USB/MSP──▶ Fusion/imu_viz.py                          │
 │   (COM4)                              │  - live attitude (roll/pitch/yaw)    │
 │                                       │  - gravity-compensated position      │
 │                                       │    estimate (accel + gyro-gated ZUPT)│
 │                                       │  - pulls in the Pi's camera stream   │
 │                                       │    and radar targets over the        │
 │                                       │    :8765 bridge and plots them       │
 │                                       │    relative to the rig               │
 │                                       └──▶ live dashboard, http://localhost  │
 │                                                                                │
 │   Firmware/motor_test.py ──USB/MSP──▶ same COM port, bench-test only        │
 │   (only one of imu_viz.py / motor_test.py can hold the port at a time)      │
 └────────────────────────────────────────────────────────────────────────────┘
```

Two things worth knowing before you dig into the code:

- **The Pi side has two modes.** You either run `CV/pi_camera_stream.py` +
  `Radar/ld2450_radar.py` as two separate processes (simpler, but they can't
  share the camera/radar with each other), or you run
  `Fusion/wallhack_dashboard.py` instead, which owns both the camera and the
  radar itself and serves one dashboard with a live person-count /
  radar-target-count cross-check. Don't run both modes at once — they fight
  over the camera and `/dev/serial0`.
- **All of the sensor math is classical, not learned.** Detection is
  MobileNet-SSD/HOG for the camera and the LD2450's own onboard tracking for
  radar; the fusion side is gravity-compensated dead-reckoning with
  zero-velocity updates (ZUPT), not a Kalman filter — good enough for a
  sanity-check overlay, not survey-grade.

See [`Docs/architecture.md`](Docs/architecture.md) for the full data-flow
breakdown (wire protocols, frame formats, ports) and
[`Docs/setup.md`](Docs/setup.md) for getting a fresh Pi/PC talking to each
other.
