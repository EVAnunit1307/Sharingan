# Camera/radar → laptop → native Quest people

The Pi captures camera frames, identifies people and reads radar. The laptop
matches observations, serves the existing sensor dashboard through read-only
proxies, and sends a `spatial_people` extension to the native Quest app.
This version assumes a stationary, rigid, level camera/radar combo with parallel
sensor axes and a shared target floor plane. It does not track a flying drone.

## Start the Pi and laptop

Use the merged Pi setup in [SensorRig/HANDOFF.md](../SensorRig/HANDOFF.md), with
this branch's telemetry additions deployed on the Pi. From the repository root:

```sh
# Pi (with its camera/radar dependencies installed)
python3 SensorRig/CV/pi_camera_stream.py --radar --quest-port 8765

# Laptop (Python 3.10+ recommended; validated with Python 3.12)
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r GroundStation/requirements.txt
python -m GroundStation.server --pi-http http://<pi>:8766 --pi-websocket ws://<pi>:8765/
```

On Windows, activate with `.venv\Scripts\Activate.ps1`. The laptop does not
need camera, OpenCV, ONNX or serial dependencies. `--pi-websocket` defaults to
port 8765 on the Pi dashboard host. The new mode needs no externally supplied
Pi world pose; Quest controller registration provides that relationship.

Open `http://<laptop>:8766`. The Pi and laptop may use the same port numbers on
their separate hosts. Quest connects to the **laptop's** WebSocket on port 8765.
Keep all three devices on a reachable local network. Bind a different address
or ports using `--host`, `--port` and `--quest-port` when needed.

## Measure and verify alignment

1. Configure and confirm the radar mount using the Pi dashboard. Retain its
   existing raw-X mirror, mount yaw and reference offset correction.
2. In the laptop dashboard, open **Camera / radar alignment**. Enter the measured
   horizontal offset from the configured radar reference origin to the camera,
   in metres: positive right and positive forward in the reference axes.
3. Put one person left, centre and right in view, at measured distances. Check
   camera boxes and corrected radar bearings/positions. Both sensors must be
   level and face the same direction. Verify the configured camera HFOV against
   the actual inference image (`--hfov`, default 62 degrees).
4. Enable **I verified left / centre / right alignment**, then save. Matching is
   disabled by default; valid camera estimates can still appear as ESTIMATED.

Configuration persists in `Saved/GroundStation/fusion.json`; use `--config` to
choose another file. Matching thresholds can also be set there. The laptop
proxies cannot change the Pi mount or start radar trials. Change those directly
on the Pi dashboard.

Parallel sensors share the Pi mount yaw. Matching subtracts the measured camera
origin and rotates corrected radar positions back into camera axes. Camera
estimates rotate into the reference axes and add the camera offset. Corrected
radar output positions are never mirrored, rotated or translated a second time.
If the Pi uses a nonzero reference offset/yaw, Quest placement must mark that
**configured origin and forward axis**, not an uncorrected sensor axis.

## Launch and register Quest

Launch the native app with:

```text
-WallhackSensorPeople -WallhackBridgeUrl=ws://<laptop>:8765/
```

After telemetry arrives, point the right controller at the floor directly below
the configured radar reference origin; press **right trigger**. Point at a floor
point at least **0.5 m** along that reference's forward direction and press the
trigger again. The app creates a session spatial anchor. Use **A** to cancel or
restart placement, and **B** to cycle minimal, hidden and full HUD visibility.

The app displays at most eight generic 1.65 m standing silhouettes, facing the
sensor: green **RADAR** for a confirmed association, amber **ESTIMATED** for
camera range. Their height, posture and facing are display assumptions. Camera
confidence determines priority, with camera ID as the tie-breaker. Unclassified
radar targets remain blue map dots; displayed radar-matched people have no
duplicate dot. The full HUD shows wearer-relative horizontal range and bearing.

Tracking or anchor loss hides world silhouettes. Recenter, app resume/restart,
source-process restart or reference changes invalidate placement. After any
physical rig movement, press **A** and place the reference again; physical rig
movement is not detected automatically. Nothing is persisted across sessions.
Manual people placement/navigation and synthetic demo contacts keep their own
stores and modes.

For an Unreal desktop preview, also supply `-WallhackSensorPeoplePreview -nohmd`.
Use WASD and arrow keys to move/look, Enter to mark a floor point and C to restart.
This bypasses the headset/anchor requirement explicitly and does not validate
Quest tracking, stereo rendering or passthrough.

## Recording and replay

Record incoming Pi packets without overwriting an existing file:

```sh
python -m GroundStation.server --pi-http http://<pi>:8766 --record Saved/sensor-run.jsonl
```

Replay them at their recorded arrival intervals through the same live Quest
WebSocket endpoint:

```sh
python -m GroundStation.server --replay Saved/sensor-run.jsonl --config Saved/GroundStation/fusion.json
```

The dashboard and native HUD label replay. Offline replay does not provide
video/raw-dashboard proxies. The last observation expires normally at the end.
Recordings contain `{ "received_at": <laptop monotonic seconds>, "packet": {...} }`
per line. They must contain full source packets, not just `spatial_people`.
For a deterministic file-only transform, use:

```sh
python -m GroundStation.replay Saved/sensor-run.jsonl --config Saved/GroundStation/fusion.json
```

That command writes augmented JSONL to stdout and opens no sockets or hardware.

## Matching and freshness

- Buffer at most one second / 128 radar frames. Process each distinct camera
  frame once against the nearest buffered radar capture within 150 ms.
- Require one-to-one mutual-nearest bearing matches, at most 8 degrees centre
  error, projection inside the person box expanded by 3 degrees, and at least
  3 degrees separation from the next eligible candidate in both directions.
- Require the same association on two distinct camera frames. Ambiguity,
  reconnect generations, mounting or camera geometry changes reset confirmation.
- Preserve camera identity when switching position source. Clipped boxes have
  no camera depth fallback. Radar alone cannot create a classified person.
- Camera expiry is 750 ms; radar expiry is 500 ms. Count Pi observation age,
  laptop buffer/relay residence and Quest residence. Duplicate frames never
  renew expiry. Radar expiry downgrades to a still-fresh camera estimate.
  Fresh empty camera frames remove absent people immediately.
- Pi capture timestamps are compared only within a Pi session, never against
  laptop/Quest clocks. Network transit time is not clock-synchronized or measured
  by this version. Queues are bounded, but latency and alignment still require
  measurement on the deployment network.

## Packet contract

Existing version-one Pi fields remain present. Additions on the Pi are:

| Field | Meaning |
| --- | --- |
| `source_session_id` | New opaque identity per sensor process. |
| `camera_generation` | Camera reconnect/reset generation. |
| `camera_capture_mono_ms` | Pi monotonic capture timestamp. |
| `camera_frame_width`, `camera_frame_height`, `camera_hfov_deg` | Actual inference geometry. |
| `radar.generation`, `radar.capture_mono_ms` | Radar reconnect generation and measurement time in the same Pi clock domain. |

The relay adds `relay_session_id`, increasing `relay_sequence`, `is_replay`, and
`spatial_people` with `version: 1`, `units: "m"`,
`coordinate_frame: "sensor_reference_2d"`, source/reference identity, top-level
camera frame/generation/age, association status, counts, `people` and
unclassified `radar_targets`. `reference_id` fingerprints mount and camera offset
configuration so the native app can invalidate its registration.

Each person carries camera ID/generation/frame, confidence, right/forward metres,
`position_source` (`radar_matched` or `camera_estimate`), camera age, optional radar
ID/generation/frame/age and optional `fallback_position`. The full person identity
is source session + camera generation + camera ID. Camera scores do not represent
radar confidence or certainty that a bearing association is correct. See the
[synthetic relay fixture](tests/fixtures/spatial_people.json) consumed by Unreal tests.

## Validation status and remaining work

On this Mac: **46 ground-station tests and 43 Pi tests pass** with Python 3.12.
These include real loopback WebSockets, HTTP/config/proxy checks, and actual Pi
snapshot → bridge packet → fusion matching/fallback. No physical sensors are
opened by those tests.

```sh
python -m unittest discover -s GroundStation/tests -v
# Requires SensorRig/CV/requirements.txt in the test environment:
python -m unittest discover -s SensorRig/CV/tests -v
```

Four `Wallhack.SensorPeople.*` Unreal automation tests were added for the shared
packet fixture, expiry/order, coordinates/viewer motion, and renderer source
changes/removal. They have **not been compiled or run here**: this Mac has no
Unreal installation or connected Quest. Run the existing full suite and Android
cook on the Unreal/Quest development machine:

```powershell
./Build/run_navigation_tests.ps1
./Build/package_navigation.ps1
```

The first script runs all `Wallhack.*` tests, including manual placement and
navigation regressions; the second performs a full Android ASTC build/cook.
Before acceptance, replay recorded real packets into Unreal, then follow the
[headset checks](../Docs/quest-ground-station-integration.md). No APK, stereo,
passthrough or physical alignment result is claimed by this implementation.
