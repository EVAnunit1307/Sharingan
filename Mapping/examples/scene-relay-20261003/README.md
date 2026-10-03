# Ground-station replay from saved couch footage

Open **[the offline replay](index.html)**. Choose a connection and scrub the time
slider, or choose **View interruption**. The last complete map stays visible when
updates stop. Recovered revisions replace it only after every piece is checked.
The amber path and hollow endpoint are historical; current camera position remains
withheld because these AI batches are already several seconds old.

Evan clarified that work should continue on saved footage until new recording is
needed. This step reuses the successful [streak-only selection](../selection-followup-20261003/README.md).
It adds a transport-independent receiver and deterministic replay, without a new
capture or model call. It does not change the existing live Pi/Quest protocol.

## Results

Times are relative to the first selected image. They come from a simulation with
measured warm model times, not a live radio or streaming benchmark.

| Assumed application link | First map received | Latest map received | Map revisions installed | Final recorded poses |
| --- | --- | --- | --- | --- |
| 2 Mbit/s, 60 ms base delay | 36.86 s | 58.29 s | 1, 2, 3 | 40 |
| 256 kbit/s, 120 ms base delay | 43.25 s | 68.89 s | 1, 2, 3 | 40 |
| 512 kbit/s, 150–350 ms delay, 5% random loss, interruption at 43–53 s | 40.75 s | 64.54 s | 1, 3 | 40 |

The interrupted case retains revision 1 through the outage, then skips the
incomplete second revision and installs revision 3. Its 208 dropped packets
include 183 outage drops and 25 random drops. The repaired map still has all
40 selected camera estimates. Old/duplicate data cannot roll back the map.

The three batch model calls were 7.110, 7.568 and 7.695 seconds. Adding those to
their final observation times makes the batches available at 35.89, 46.43 and
56.72 seconds in this optimistic schedule. All received camera estimates exceed
the two-second display-freshness timeout; **zero samples show a current marker**.
Even on the clear link, the last map arrives with its latest observation 9.27 s
old. A useful slow room draft and a useful live pose stream have different timing
requirements. No faster estimator is validated here.

## What is included

- [input.json](input.json): three complete scene snapshots, recorded pose times,
  measured model times, first-camera display basis, source replay/checkpoint hashes
  and embedded reference images. Snapshots have 3,602 / 4,804 / 6,000 points,
  uniformly sampled from the accepted replay for this display/transport budget.
- [summary.json](summary.json): timing, queue, freshness and delivery measurements.
  `clear.json`, `limited.json`, and `interrupted.json` contain every sampled
  receiver state and significant delivery event.
- [Reference image 1](images/section-1.jpg), [2](images/section-2.jpg), and
  [3](images/section-3.jpg); [interruption screenshot](renders/outage.png) and
  all three final renders plus mobile in `renders/`.
- `tests.txt` and `SHA256SUMS.json`: verification record and artifact hashes.

The receiver buffers at most one partial snapshot. In this run the sender queues
at most 215 map pieces and two priority messages. Partial updates never replace
the displayed map. A wrong session/frame/clock/map identity is rejected; damaged
snapshots fail SHA-256 and schema/geometry checks. Capture-time expiry applies
even after a delayed packet or reconnect. The [receiver contract](../../../Docs/recorded-scene-relay.md)
documents the identity, time and memory bounds.

## Reproduce without Pi or model weights

From the repository root, Python 3.10+ and its standard library are sufficient:

```sh
python3 -m Mapping.scene_relay_trial run --output Saved/scene-relay-repeat
python3 -m unittest GroundStation.tests.test_scene_transport Mapping.tests.test_scene_relay_trial
```

Open `Saved/scene-relay-repeat/index.html`. The output directory must be new.
The default input is this bundled `input.json`. All three scenario JSON results
were reproduced exactly with Python's `-S` flag, which disables site-package
loading. SHA-256 for the input is recorded in `summary.json`.

To regenerate this input from inference rather than reuse completed geometry,
first reproduce the [selection experiment](../selection-followup-20261003/README.md).
Its original selected images, camera calibration and model/source pins are
already published. Then, in the environment with NumPy:

```sh
python -m Mapping.scene_relay_trial prepare --replay Saved/selection-repeat/streak_only-replay/replay.json --trials Saved/selection-repeat/streak_only/window-0 Saved/selection-repeat/streak_only/window-1 Saved/selection-repeat/streak_only/window-2 --output Saved/scene-relay-new-input.json
python -m Mapping.scene_relay_trial run --input Saved/scene-relay-new-input.json --output Saved/scene-relay-new-run
```

New model timings may change the schedule. No geometry is refit by the relay.
The upstream model is Depth Anything 3 Large, pinned in the source bundle, whose
checkpoint is **CC BY-NC 4.0**. No checkpoint is included here. The source replay
hash identifies the exact retained draft; those predictions remain unverified
and arbitrary-scale, with no IMU or physical reference poses.

## Simulation limits

The link carries **already ground-computed scenes to a display/relay consumer**.
It does not show the Pi computing these maps and does not replace the camera
uplink. The viewer images are local reference assets; their bytes are not sent.

The producer schedule includes each batch's final observation plus measured warm
model time. It excludes image transfer, imports/load, preprocessing, alignment
and serialization CPU. Network cost includes JSON envelopes, base64 payloads,
heartbeat and repair messages on one serialized bidirectional application link.
It excludes modem/network framing and FEC. Heartbeat and repair messages are
prioritized; repair requests can also be dropped. Random seed 73 is fixed.

Clocks have an exact known relationship in the simulation. Real radios, clock
synchronization, producer restart/rebinding, display registration and Quest
hardware still need integration checks. No people contacts or flight commands
are sent. The two-second timeout is an experimental display setting, not a
flight-control guarantee. Room accuracy and new-room repeatability still require
independent physical references.

Validation: 62 focused ground-station/replay tests passed, including 16 new
receiver/simulation tests. Browser checks cover every timeline sample in all
three profiles, no early/partial map display, historical-marker labeling, retained
map during outage, recovery, source images, playback, mobile width and offline
loading. Final desktop/outage and mobile screenshots were inspected.
