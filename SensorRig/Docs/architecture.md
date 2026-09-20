# Sensing architecture

```text
IMX219 -> Picamera2 BGR capture (24 fps) -> latest distinct frame
                                                  |
                                             YOLOX nano (up to 10 fps)
                                                  |
                                        confirmation + timed expiry
                                                  |
                             matched-frame annotated MJPEG + detection JSON
                                                  |
LD2450 UART -> bounded parser -> confirmed radar targets (separate)
                                                  |
PC IMU rig pose -> optional WebSocket bridge -> Quest telemetry HUD
```

`CV/camera_dashboard.py` is the shared runtime. Camera-only is the default;
`Fusion/wallhack_dashboard.py` starts it with radar enabled. Inference always
uses the latest distinct frame, avoids queues, and annotates that exact frame.
The detector loads relative to its own directory, not the current shell directory.
The experiment launchers use the same detector implementation.

| Port | Service |
|---|---|
| 8766 | Dashboard, `/stream`, `/snapshot.jpg`, `/detections`, `/healthz`, `/radar.json` |
| 8765 | Optional WebSocket (`--quest-port 8765`) |
| 8767 | Shared standalone radar page; do not share its UART with combined mode |
| 8768 | Experimental `sensor-test/live_detect.py` launcher |
| `/dev/serial0`, 256000 baud | LD2450 radar UART |
| `COM4`, 115200 baud | PC flight-controller MSP in `imu_viz.py` |

All clocks used for expiry are monotonic. JSON also includes wall-clock capture
timestamps. Camera results expire after 750 ms, unmatched person tracks after
350 ms, radar measurements after 500 ms, and incoming rig pose after one second.
Counts only include observed people; held tracks are separately identified.

Camera bearing uses pinhole geometry and 62° assumed horizontal FOV. Forward
range estimates assume an unclipped 1.65 m standing person. The camera cannot
measure range directly or infer a person's height/posture from a box; clipped
boxes get bearing only. Radar `right_m`/`forward_m` use metres; legacy `x`/`y` use millimetres.
The radar origin and forward-facing mount are operator configured. The independent
`/radar` page and `drone_relative_radar_targets` bridge array stay available without
camera observations or world pose. Physical accuracy and drywall performance are
unverified; see [the radar procedure](../Radar/README.md).
The bridge converts available camera estimates to world metres using the incoming
rig pose. Radar remains separate until alignment and association are calibrated.
This is not a calibrated multisensor position filter.

The WebSocket sends the existing Unreal `rig` + `detections` schema. It clears
contacts if the camera or pose is stale. `--stationary-rig` explicitly enables a
fixed sensor-local bench frame. The Quest app selects telemetry with
`-WallhackBridge` and accepts `-WallhackBridgeUrl=ws://host:8765/`; normal navigation
mode is still the default. Room-anchored overlays need rig/headset registration.

The PC IMU utility still owns attitude integration and motor controls. It sends
metres and clockwise heading to the bridge; its dead-reckoning can drift. The
camera service does not operate motors or change the flight controller.
