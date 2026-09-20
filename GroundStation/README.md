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

# Laptop (Python 3.10+ recommended; validated with Python 3.12 and 3.14)
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r GroundStation/requirements.txt
python -m GroundStation.server --pi-http http://larp-pi.local:8766 --pi-websocket ws://larp-pi.local:8765/
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
   disabled by default; camera estimates appear as ESTIMATED and independent
   radar returns appear as RADAR ONLY without requiring a camera match.

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
camera range, and blue **RADAR ONLY** for independent radar returns. Either
sensor can supply a silhouette; there is no camera-and-radar display gate.
Height, posture and facing are display assumptions, and radar-only bodies do
not claim a camera-confirmed person. Camera identities use C and radar identities
use R, so C1 and R1 remain distinct. Camera confidence determines priority, with
camera ID as the tie-breaker, followed by unmatched radar returns. The body limit
is eight in total; overflow radar returns remain map dots. Confirmed matches
produce one body, without a duplicate radar body or dot. Until alignment and a
match are established, independent readings may represent the same real person.
The full HUD shows wearer-relative horizontal range and bearing.

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
  Fresh empty camera frames remove absent camera contacts immediately;
  independent fresh radar contacts remain. A brief upstream receive timeout
  keeps the socket open while observations expire normally. A five-second
  packet stall or a closed connection causes reconnection.
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

On the Windows integration branch: **48 ground-station tests and 44 Pi tests pass** with Python 3.14.6.
The previous 46-test ground-station suite and the unchanged 44 Pi tests also
passed on the physical Pi with Python 3.13.5.
These include real loopback WebSockets, HTTP/config/proxy checks, and actual Pi
snapshot → bridge packet → fusion matching/fallback. No physical sensors are
opened by those tests.

```sh
python -m unittest discover -s GroundStation/tests -v
# Requires SensorRig/CV/requirements.txt in the test environment:
python -m unittest discover -s SensorRig/CV/tests -v
```

All **79 `Wallhack.*` Unreal tests pass**, including four sensor tests for the
shared packet fixture, expiry/order, coordinates/viewer motion, and renderer
source changes/removal. The full suite and Android cook commands are:

```powershell
./Build/run_navigation_tests.ps1
./Build/package_navigation.ps1
```

The first script runs all `Wallhack.*` tests, including manual placement and
navigation regressions; the second performs a full Android ASTC build/cook.

For the additional software integration check, install the Pi requirements,
`opencv-python-headless`, and `SensorRig/CV/requirements-dev.txt` in the test
environment. This test uses synthetic observations and actual local servers;
it never opens sensor hardware or changes its saved calibration:

```powershell
python Build/verify_sensor_setup.py `
  --browser 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe' `
  --unreal 'C:/Program Files/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
```

Build the editor first. Without optional arguments the harness checks ONNX
loading/inference, camera/radar processing, and real Pi-to-relay WebSockets.
`--browser` checks both dashboards and failure handling; `--unreal` runs the
additional `SensorSetup.NativeRelay` automation test through the real native
socket, controller registration and people actor. All passed on this branch.
Results and screenshots go to `Saved/PiSensorPullTest/Integration`.

The full local results and remaining physical acceptance steps are recorded in
the [integration report](../Docs/quest-ground-station-integration.md). The updated
Pi passes live camera/radar transport checks; the verified APK is installed on
Quest and its relay connection was checked. The wearer confirmed reference
placement and a visible live ESTIMATED silhouette on the preceding build.
Physical alignment, both-eye visibility and the new independent-radar mode
remain pending the next wearer test cycle.
Sensor mode displays contacts; manual navigation remains a separate
mode and does not route to the live sensor contacts yet.
