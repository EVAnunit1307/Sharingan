# Combined camera and radar dashboard

The camera dashboard uses the same YOLOX nano detector as the offline evaluation.
It detects the person class across body orientations; it does not require a visible
face or estimate which way someone is facing. Run the camera first, inspect it in a
browser, then enable radar and the Quest bridge.

From the HTN2026 workspace:

```sh
python3 -m venv --system-site-packages .venv
.venv/bin/pip install -r Sharingan/SensorRig/CV/requirements.txt
.venv/bin/python Sharingan/SensorRig/CV/pi_camera_stream.py
```

Open **http://larp-pi.local:8766/** (current Pi IP: **172.20.10.3**).
The main page now places camera and radar side by side. `/quest` uses the same
live data with larger controls and reduced detail for the headset browser.
Mounting, diagnostics and trial recording remain in the combined page's Bench
tools. See [the Quest handoff](../Docs/quest_handoff.md) for connection checks and
the native integration contract.
The default source is the IMX219 CSI camera at 640×480, 24 capture fps,
180° mounting rotation, and up to 10 detection fps with two inference threads.
Use `--rotation 0` for an upright mounting, `--source 0` for a USB camera,
or `--source http://host/raw-stream` for an unannotated network stream.
`--detect-fps`, `--threads`, `--config`, `--hfov`, and `--port` are configurable.
A missing model fails visibly; there is no silent switch to HOG or a weaker model.
Only one process may own the CSI camera.

## What the dashboard shows

- Green: confirmed person observed in this frame, with ID and model confidence.
- Amber: unconfirmed candidate. Confidence is not a calibrated accuracy measure.
- Grey: briefly lost track, held for at most 350 ms and excluded from observed count.
- Capture/detection fps, inference time, and age since capture. The displayed video
  is the exact inferred frame, so boxes cannot drift over a newer camera image.
- Live radar and Quest status when those features are enabled.

Capture uses a single latest-frame slot. Inference skips backlog and consumes each
sequence only once. Stale frames clear people from JSON and replace the video with
a waiting image after 750 ms. The browser also clears observations on connection
loss. A network stream must itself deliver fresh frames; reconnecting to an upstream
server that repeats an old image cannot establish that image's original capture age.

Picamera2's `RGB888` array is already BGR in memory, suitable for OpenCV and YOLOX.
Do not apply an additional RGB→BGR conversion. See the
[official Picamera2 manual](https://datasheets.raspberrypi.com/camera/picamera2-manual.pdf).
YOLOX uses raw 0–255 BGR values, aspect-preserving 416×416 padding, and person-class
objectness×class score; see [upstream preprocessing](https://github.com/Megvii-BaseDetection/YOLOX/blob/main/yolox/data/data_augment.py).

## Validate orientation and false positives

Walk through front, side, back, partial body, entering/leaving the frame, and an
empty room. Try two people crossing and different lighting too: the saved clips
cover only one person in one room and have presence labels, not bounding boxes.

```sh
.venv/bin/python Sharingan/SensorRig/CV/evaluate.py sensor-test/evalset \
  --rotation 180 --legacy-channel-swap \
  --overrides sensor-test/reports/label_overrides.json \
  --out sensor-test/reports/camera_evaluation.json
.venv/bin/python -m unittest discover -s Sharingan/SensorRig/CV/tests -v
```

The original recorder swapped red/blue and left rotation unset; the explicit flags
correct those historical files without rewriting them. Eleven clearly empty walking
frames were visually reviewed and corrected in a separate overrides file. New
captures mark correct channel order per frame. Strong observations ≥0.75 confirm
immediately; weaker observations need two hits above 0.50 in five distinct frames.
Confirmed tracks can sustain at 0.40. Tracking expires by elapsed time, not CPU speed.

Development result on 525 saved frames: 370/379 person-present frames detected
(97.6%); 0/146 empty frames detected. Front near/mid, back, and partial-edge clips:
100%; profile: 59/60; walking: 56/64. Held boxes are excluded from these detection
metrics. This data was used during tuning and is not an independent accuracy test.
The earlier 95.9% number used different labels and counted held boxes; do not treat
the difference as a controlled model improvement. Model weights were retained;
preprocessing, tracking, and runtime were corrected, not retrained.

## Radar, then Quest

For the immediate visual handoff, open **http://172.20.10.3:8766/quest** in the
Quest browser on the same LAN. It shows camera and radar together in a browser
window; no APK rebuild is needed for this view. The native
Unreal telemetry HUD is a separate integration described below.

```sh
.venv/bin/python Sharingan/SensorRig/CV/pi_camera_stream.py \
  --radar --quest-port 8765
```

Open **http://172.20.10.3:8766/radar** for the independent top-down radar map.
The upright, forward-facing bench mount uses the radar origin, zero yaw/offsets,
and X inversion in `radar_config.json`, corrected by the physical left/right check. The page exposes metre coordinates,
raw versus filtered targets, mounting configuration and labelled drywall trials.
See [Radar/README.md](../Radar/README.md) for the test procedure and limitations.

Radar reports targets, not verified people. Through-wall performance and physical
position accuracy remain unverified. Three consecutive observations confirm a
track; missing packets clear positions after 500 ms. The radar view and
`drone_relative_radar_targets` WebSocket array do not require a camera observation
or world pose. `Fusion/wallhack_dashboard.py` now uses this same mounting config.

The optional WebSocket listens at `ws://172.20.10.3:8765/`. The existing PC
`Fusion/imu_viz.py` supplies `rig: {x, y, heading_deg, tracking_ok}` in metres and
clockwise degrees from forward. Without a fresh valid pose, world contacts are
empty. `--stationary-rig` explicitly opts into a fixed sensor-local bench frame.
Unclipped boxes have an approximate depth assuming a 1.65 m standing person;
clipped bodies have bearing only and are not exported as invented world positions.
This does not infer body yaw, posture, identity, or vitals. Radar remains a separate
measurement until alignment and association are validated.

Launch a rebuilt Quest app with:

```text
-WallhackBridge -WallhackBridgeUrl=ws://172.20.10.3:8765/
```

This opts into the telemetry HUD; the default navigation HUD remains available
without `-WallhackBridge`. The bridge sends `rig` and `detections` with `id, x, y,
conf`, matching `WallhackTelemetrySubsystem`. It additionally includes camera
boxes, timestamps, radar status and a separate drone-relative radar array.
The existing native HUD does not yet render that new radar array; use the radar
browser page for the current visual handoff. Camera coordinates require
registration to the headset's world before using them as room-anchored geometry.
No Quest or Unreal toolchain is attached to this Pi, so native build/deployment and
in-headset alignment must be verified on the Quest development machine.

## Endpoints

| Path | Purpose |
|---|---|
| `/` | Combined live camera, radar and bench controls |
| `/quest` | Combined view with larger controls for the headset browser |
| `/handoff.json` | Connection manifest, coordinate contract and validation status |
| `/stream` | Annotated MJPEG; frame IDs in each part |
| `/snapshot.jpg` | Current annotated frame or waiting image |
| `/detections` | Fresh observations, tracks, timing, camera/radar/Quest state |
| `/healthz` | 200 for fresh camera results, 503 otherwise |
| `/radar` | Independent 2D radar map and drywall trial recorder |
| `/radar/targets` | Radar coordinates, raw targets, timestamps and mounting |
| `/radar/config` | POST complete mounting configuration |
| `/radar/trials` | GET reports; POST labelled trial |
| `/radar.json` | Compatibility camera counts and radar positions in millimetres |

`detector/` contains the shared model registry, preprocessing, tracker, config, and
existing ONNX weights. `camera_dashboard.py` handles capture and HTTP;
`radar_service.py` handles optional UART; `quest_bridge.py` handles the optional
WebSocket. `evaluate.py` imports the production detector. `sensor-test/` launchers
and decoder imports forward to the same production implementation.
