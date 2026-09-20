# LD2450: stationary 2D position and drywall trials

The combined camera/radar dashboard is **http://172.20.10.3:8766/**, with a
headset layout at `/quest`. The dedicated diagnostic page remains at `/radar`.
Actual UART targets update independently of camera visibility in all views.
See [the Quest handoff](../Docs/quest_handoff.md) for connection and integration details.

## What through-wall detection means here

Detection through some interior drywall is physically plausible, but accurate
LD2450 tracking through your wall is **not yet demonstrated**. TI describes drywall
penetration for its own mmWave devices; that supports trying this experiment, not
an LD2450 performance guarantee. See [TI's application brief](https://www.ti.com/document-viewer/lit/html/swra766).

Hi-Link specifies ordinary indoor tracking, up to three targets, approximately
10 reports/s, 6 m maximum detection distance, and ±60° azimuth. **6 m is not a
through-wall range specification.** Its manual warns about moving clutter, strong
reflectors, mounting vibration and rear-lobe detections. A person behind the
sensor can produce a misleading forward contact. Its range-bin field is not a
position-error bound. See the [manufacturer's product page and manuals](https://www.hlktech.com/cn/Goods-226.html).

Our engineering expectation is that a plain drywall path may work at short
range, while layers, metal studs/foil/mesh, oblique incidence and reflections can
reduce detection or displace a reported position. The outward and return signals
both cross the wall. There is no documented UART "through-wall mode" to enable;
the normal interface supplies already-processed targets, without raw radar samples
or a per-target confidence score. Software cannot recover a target that the sensor
never reports. Stable dots alone do not establish that they represent a person
behind the wall. Motion and standing-still performance must be tested separately.

## Current bench setup

The operator specified a stationary sensor, upright board, antennas facing the
drone's nose, with the radar as the origin. `CV/radar_config.json` therefore uses
zero yaw/offsets and X inversion, corrected after the operator observed reversed left/right. Upright is interpreted as level for this bench
setup; verify it physically. This configuration is not an accuracy calibration.

- `right_m`: metres toward the drone's right; left is negative.
- `forward_m`: metres toward the drone's nose.
- `(0, 0)`: radar antenna reference, not yet the drone's center.
- Example **illustration, not a live measurement**: `(0.5, 2.0)` means 0.5 m right
  and 2 m forward of the radar.

The mounting panel persists clockwise yaw, right/forward offsets, optional mirror,
range limits, and mounting confirmation. A later drone-center origin uses the
sensor's measured displacement from that center. `drone_position_m` is withheld
when mounting is unconfirmed or not level. Raw measurements remain available.
A manual left/right test at tape-measured marks must check the sign convention.

This is a **level-sensor 2D estimate**: elevation is unobserved. Unknown target
height, roll/pitch and multipath can bias horizontal position. Mounting geometry
alone does not validate accuracy. It is not ready for a flying or vibrating drone;
there is no ego-motion correction, and IMU yaw alone cannot correct all of this.

## Run and inspect

From the HTN2026 workspace, using the existing virtual environment:

```sh
.venv/bin/python Sharingan/SensorRig/CV/pi_camera_stream.py --radar --quest-port 8765
```

Only one process may own `/dev/serial0` (256000 baud). Both UART directions must
be connected for diagnostic queries: radar TX to Pi RX, radar RX to Pi TX, and
common ground. Follow the manufacturer's power/logic wiring specification.
An optional **radar-only alternative**, after stopping the combined dashboard:

```sh
.venv/bin/python Sharingan/SensorRig/Radar/ld2450_radar.py
```

That serves the same map and API on port 8767 without opening the camera.
Both entry points share parsing, tracking, configuration and trial recording.
A read-only startup transaction queries firmware, tracking mode and zones, then
exits configuration mode. It does not flash firmware or rewrite sensor settings.
On this connected sensor it returned **V2.04.23101915**, **multiple**, **zones disabled**.
Stationary-person capability of this installed firmware has not been measured.

| Endpoint | Result |
|---|---|
| `GET /radar` | Top-down map, raw crosses, filtered tracks, diagnostics and trials |
| `GET /radar/targets` | Fresh raw/filtered targets, metre coordinates, timestamps and mounting |
| `POST /radar/config` | Validate and atomically persist the full configuration object |
| `GET /radar/trials` | Recorder status and last 20 reports, restored after restart |
| `POST /radar/trials` | Start an explicitly labelled trial after a five-second countdown |

The existing `/radar.json` combined endpoint retains millimetre fields. In the
new API `right_m`/`forward_m` are metres; legacy `x`/`y` and raw fields are mm;
`spd` is cm/s and `radial_speed_mps` is m/s. Do not mix these units. The radar's
three hardware slots are not stable person IDs; software IDs are local tracks
and can change or swap during gaps/crossings. They reset after reconnect/restart.

Three consecutive observations confirm a track. Position filtering uses an EMA
with alpha 0.5; raw values are retained to expose its lag and jitter. Missing
observations are not rendered as current contacts. No packets for 500 ms clears
all coordinates, and the browser clears contacts on lost HTTP connectivity.
Range filtering is radial; valid wide-angle observations beyond ±3 m sideways
are retained. Radar confirmation is not human classification.

The WebSocket on 8765 includes `drone_relative_radar_targets` even if the camera
sees nothing or world pose is unavailable. Fields include `right_m`, `forward_m`,
`source: "ld2450"`, `classification: "unverified_radar_target"`, and null confidence.
These contacts stay separate from camera `detections`. The existing native Quest
HUD does not yet render this new array; the browser map is the working view.
World registration, cross-sensor association and native rendering remain separate.

## Physical validation, in order

1. Fix the board upright and level, with the antennas unobstructed and facing
   perpendicular to the drywall. Mark `(0, 2 m)` and points 0.5–1 m left/right
   where practical. Measure from the radar, including the distance to the wall.
2. Before placing a wall in the path, check the reported left/right direction and
   distance against those marks. Change mirror only if the physical test shows
   it is needed. Keep the sensor fixed for every comparison possible.
3. Select **Empty scene** and record a baseline with nobody in the coverage area,
   including behind the sensor. Remove moving fans and nearby moving objects.
   A nonzero target fraction here flags clutter/false detections to investigate.
4. Select **One person — clear path**, enter measured coordinates, record while
   shifting gently around the mark. For a removable drywall panel, insert it
   without moving the sensor/mark; with a fixed wall use a comparable clear-path
   geometry and document that it is not an identical A/B setup.
5. Select **One person — behind drywall**, enter the actual coordinates, and
   record. Repeat at center/left/right and several distances. Compare detected
   frame fraction, multiple-target ambiguity, and coordinate error with baseline.
6. Repeat while standing still and note which trial IDs use that condition.
   Repeat the empty trial afterward. A disappearing dot does not prove absence.

The UI records 12 seconds after a five-second countdown. JSON reports in
`Radar/trials/` retain operator labels, raw and filtered data, the mounting
configuration and summaries. Labels are not independent ground truth.
Position error is calculated only on single-target frames; missed detections and
multiple targets are reported separately. No fresh frames yields a null result,
not a successful empty baseline. No automatic "through-wall verified" claim is
made. Configuration changes during recording invalidate the trial.

Decide the acceptable error before accepting the experiment (for example, 0.5 m
might suffice for a coarse dot but not for a tightly registered human overlay).
If clear-path trials work but drywall trials repeatedly fail or produce large
errors, this module/setup cannot satisfy that wall requirement. Additional UI
smoothing will not repair it; use hardware/firmware designed and validated for
that material and positioning requirement.
