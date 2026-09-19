# Fusion — combining camera, radar, and IMU into one picture

Two scripts live here, one per side of the WiFi link:

## `wallhack_dashboard.py` (runs on the Pi)

The Pi-side fusion point. Instead of running `CV/pi_camera_stream.py` and
`Radar/ld2450_radar.py` as two separate processes, this owns the camera
**and** the radar serial port itself and serves one dashboard showing both
side by side, plus a live person-count vs. radar-target-count cross-check —
useful for sanity-checking that the camera and radar agree on how many
people are actually in frame.

```
python3 wallhack_dashboard.py
```

then open `http://larp-pi.local:8766/` (or `http://<pi-ip>:8766/` if mDNS
isn't resolving).

**Don't run this alongside `CV/pi_camera_stream.py` or
`Radar/ld2450_radar.py`** — all three fight over the same camera and
`/dev/serial0`.

Detection here is upgraded from CV/'s MobileNet-SSD/HOG to **YOLOv8n**
(ONNX export, 640×640 input, full COCO 80-class), specifically because
MobileNet-SSD's furniture-vs-person confusion was showing up often enough to
matter. YOLO inference runs in its own thread, decoupled from the
capture/encode/stream loop — YOLOv8n on a Pi 4 CPU can take 300ms–1s+ per
frame, and running it inline was stalling the MJPEG stream for that entire
window (from the browser it just looked like `/stream` hanging). The
capture loop now always runs at full camera framerate and draws whatever
the latest finished detection result was, however many hundred ms old.

Each detected person also gets a **rough monocular position estimate**
(`estimate_person_position()`) so it can be plotted on the same X/Y scope as
the radar targets: bearing comes from the box's left/right position in
frame (reliable), distance comes from assumed average adult height vs. box
height (a rough guess — nowhere near the radar's precision, this is for a
sanity-check overlay, not a hard number). `CAMERA_HFOV_DEG` assumes a Pi
Camera Module v2 (~62° horizontal FOV) — change it to match your camera or
both the bearing and distance math will be off.

| Endpoint | Purpose |
|---|---|
| `/` | Combined dashboard (camera pane + radar scope + cross-check) |
| `/stream` | Annotated MJPEG camera feed |
| `/radar.json` | Raw radar target JSON |

## `imu_viz.py` (runs on the PC — the ground station)

The main ground-station dashboard. Reads the flight controller's live
attitude (roll/pitch/yaw) and raw IMU over **MSP** on a serial/USB port
(`COM4` by default), and derives a rough position estimate via
gravity-compensated acceleration integration with gyro-gated zero-velocity
updates (ZUPT) — not a Kalman filter, just enough dead-reckoning to place a
marker on a minimap, and it drifts like dead-reckoning always does.

It then bridges to the Pi over a **WebSocket** (`BRIDGE_URL`,
`ws://larp-pi.local:8765/`): the drone/rig's IMU pose is sent to the Pi as
the real reference frame for its person-detection fusion (replacing an
earlier `fake_rig.py` stub), and the Pi's detections come back to be
plotted here relative to the rig. It also pulls in the Pi's raw camera feed
directly (`PI_STREAM_URL`, `http://larp-pi.local:8766/stream`) for a live
picture-in-picture pane.

Motor test-mode (arm/unlock/per-motor or all-motor throttle, the software
watchdog) is merged into this same process using the identical mechanism as
`Firmware/motor_test.py`, so you don't need a second program fighting over
the COM port if you want to spin motors while watching live attitude.

```
python3 imu_viz.py
```

| Endpoint | Purpose |
|---|---|
| `/attitude` | Live roll/pitch/yaw/position JSON |
| `/reset` | Zero the position/velocity estimate (do this on a level, stationary bench) |
| `/arm`, `/unlock`, `/motor`, `/all`, `/throttle`, `/num_motors`, `/estop` | Same motor test-mode API as `Firmware/motor_test.py` |

**Only one of `imu_viz.py` or `motor_test.py` can hold the COM port at a
time.**
