# Recorded scene relay boundary

This is a tested, file-only ground-station/display experiment. It does not change
the live Pi or Quest protocol. [Open the saved-footage demo](../Mapping/examples/scene-relay-20261003/index.html).

## Behavior

`GroundStation.scene_transport.SceneReceiver` retains the last complete map while
a new revision arrives. It installs a revision atomically after all bounded pieces
arrive, their SHA-256 matches and their geometry/metadata pass validation. Repeated
pieces are harmless. Older revisions cannot overwrite a newer map or assembly.
The receiver buffers at most one partial revision, expires it after 20 seconds,
and exposes missing indices for repair. Expiry does not erase the installed map.

Every receiver is explicitly bound to one session, map, coordinate frame and
source clock. A reconnect within that session preserves the map. A producer
restart or changed origin requires a new receiver; incoming packets cannot silently
reset it. A real transport should authenticate that setup. Checksums detect damaged
assembly, not an impersonated sender.

Camera observations have their own sequence and capture timestamp. Receipt of an
old observation, a new map, or a heartbeat does not refresh the observation's age.
A position is exposed only while tracking is true, its required map revision is
installed, its source clock is explicitly mapped to receiver time, and its age is
between zero and two seconds. Two seconds is an experimental display timeout,
not a navigation requirement. Unknown clocks, future observations, regressing
sequences/timestamps, tracking loss and expiration withhold the current marker.

The saved DA3 outputs are always too old for this timeout: their warm model calls
alone take 7.11–7.70 seconds. The viewer therefore draws only an amber historical
path and hollow historical endpoint. This is useful evidence for separating slow
room refinement from a future fast localization stream. No IMU integration or
validated fast estimator is supplied by this experiment.

## Data contract

Schema: `wallhack.recorded_scene.v1`. JSON is canonicalized with sorted keys and
compact separators when splitting snapshots; packets use newline-delimited JSON
for the simulated byte budget. All nanosecond fields are nonnegative integers
below `2**53`; the experiment uses relative time, avoiding JavaScript precision
loss on absolute timestamps. Position axes follow the original DA3 seed frame;
they are not gravity aligned, registered with Quest or metric.

| Message | Required fields beyond schema and identity | Purpose |
| --- | --- | --- |
| Snapshot body | `revision`, `observed_ns`, `points`, `poses`, `units: arbitrary`, `live: false`, `mapping_eligible: false` | Full replacement scene with timestamped historical path |
| `chunk` | `revision`, `index`, `count`, `total_bytes`, `sha256`, base64 `payload` | Assemble a bounded snapshot |
| `camera` | `revision`, `sequence`, `observed_ns`, `position`, `tracking` | Separate timestamped position; no inferred orientation or contacts |
| `heartbeat` | `revision`, `sequence`, `observed_ns` | Advertise current producer revision and recent link activity |
| `repair` (simulated sender adapter) | `revision`, `missing` indices or null for full snapshot | Request only missing pieces, or restart assembly |

Identity fields are `session_id`, `map_id`, `frame_id`, and `clock_id`. Receivers
reject any mismatch. A newer map revision must preserve its frame; use a new frame
identity and explicit reset if global optimization changes the coordinate system.
The producer must not change the scale/origin silently.

Limits: 1,000,000 decoded bytes per snapshot, 1,024 bytes per chunk, at most 10,000
points and 1,000 historical poses. Coordinates must be finite and within ±1e9
arbitrary units; RGB channels are integral 0–255. The core receives decoded Python
dicts. A future network adapter must bound raw message size before JSON parsing;
the current tool reads trusted bundled inputs and opens no sockets.

## Simulation and scope

`Mapping.scene_relay_trial` uses one serialized bidirectional application-byte
link. Small camera/heartbeat/repair messages have priority over map chunks. The
sender replaces queued older map work on a new revision, suppresses duplicate
queued pieces and retries missing chunks every two seconds when recent heartbeats
advertise incomplete data. Retry requests consume bandwidth and can also be lost.
In-flight packets can arrive late or out of order. Random seed 73 fixes the trial.

Three complete maps are sampled to at most 6,000 display points from the accepted
40-view couch replay. Producer availability is modeled as the final observation
time plus its measured warm model-call duration, with sequential model execution.
This does not re-run inference. It excludes camera uplink, preprocessing, overlap
fitting, serialization CPU and modem framing/FEC. The bandwidth and delays are
assumptions, not measurements of any radio or of an end-to-end streaming system.

The tested link is **ground-computed scene → display receiver**. This can inform
the future Quest/display adapter. It does not establish that the Pi can compute
these maps and relay them on a low-rate drone uplink. Images in the viewer are
bundled local references and are not included in the transmitted stream.

The receiver is separate from `GroundStation.server` and existing people fusion.
Research maps remain ineligible for live/metric placement. A production adapter
still needs real clocks, transport/session authentication, coordinate registration,
model/pose validation and hardware acceptance. Herman's IMU work stays separate.

## Run and check

From the repository root, Python 3.10+ with only the standard library:

```sh
python3 -m Mapping.scene_relay_trial run --output Saved/scene-relay-repeat
python3 -m unittest GroundStation.tests.test_scene_transport Mapping.tests.test_scene_relay_trial
```

Open `Saved/scene-relay-repeat/index.html`. The committed input, traces, summary,
images and renders let a teammate reproduce the simulation without Pi access,
model weights or new recordings. See the [bundle README](../Mapping/examples/scene-relay-20261003/README.md)
for the distinct inference-reproduction step.
