# ORB-SLAM3 tracking replay

Optional laptop experiment for the rough-room-map use case. This replays clean
recorded camera images, exports final optimized frame poses and visual landmarks,
and shows capture frames beside a tracking timeline. It does not yet run live,
infer a floor plan, fuse an IMU, or place people in world coordinates.

## Build on this Mac

Prerequisites: Xcode command-line tools, Homebrew `cmake` and `boost`, OpenSSL.
The script downloads pinned upstream ORB-SLAM3, OpenCV 4.11.0 and Eigen 3.4.0
under ignored `Saved/MappingResearch/orb-slam3/`. Archive checksums are verified.

```sh
bash Build/build_orb_replay.sh
```

This research executable links GPL-3.0 ORB-SLAM3; preserve its source and license
when distributing a build. Upstream is pinned to
`4452a3c4ab75b1cde34e5505a36ec3f9edcdc4c4` from
<https://github.com/UZ-SLAMLab/ORB_SLAM3>.

The CMake target compiles upstream tracking, local mapping, loop closing,
optimization and camera code. Display-only MapDrawer/Viewer implementations are
replaced with a headless adapter; attempting to open the viewer throws. Modern
standard library headers replace old GCC/TR1 headers. Three accessors are added
for stopped-worker export. `WaitForExport` waits for mapping/loop/GBA completion
and joins the local/loop workers because this upstream `Shutdown` does not wait.
No estimator thresholds or tracking algorithms are patched.

## Replay and inspect

The recording must have its own reviewed `camera-calibration.json` matching its
image geometry. Calibration is never silently copied from another recording.

```sh
.venv/bin/python -m Mapping.orb_replay --session Saved/Mapping/20261001T053033Z-f37549ec
```

Alternatively, choose **Test continuous tracking** beside a calibrated saved
recording on `/map`, then **Replay tracking**. The result page is
`/map-assets/tracking.html?session=<session_id>`.

Each run keeps settings, calibration, input frame timing, CSV outputs, logs and
`result.json` under the recording's unique `tracking/<revision>/` directory.
`tracking.json` points to the latest result by containing a copy; existing COLMAP
`scene.json` and reconstruction revisions are preserved. The settings use the
recording's actual sampling rate and 1,000 ORB features, pyramid scale 1.2/eight
levels and FAST thresholds 20/7. OpenCV integer pixel centers are passed unchanged.
Replay is paced at the recorded timestamps to give the background mapper time.

## Interpreting results

- The timeline counts all input frames, including initialization and failure.
- Only upstream state `OK` with a retained final pose displays a camera marker.
  Recently-lost predictions and poses from discarded maps are hidden.
- Corrected frame poses follow their final reference keyframes, including culled
  keyframe parent transforms. Landmarks and poses share each map's native frame.
- Independent maps have independent origins and scales. The viewer separates
  them, and never draws path lines across a tracking gap or map boundary.
- Coarse display groups landmarks into cells sized from each map's extent. This
  is display aggregation in arbitrary units, not metric occupancy or wall fitting.
- A long tracked segment is not proof of accurate geometry. Real room dimensions,
  return-to-start consistency and people placement still need physical checks.
- Existing 3 fps recordings are a preliminary test. Higher-rate timestamped
  capture and less motion blur are needed for a representative motion trial.

Python replay/export and HTTP route checks:
`.venv/bin/python -m unittest Mapping.tests.test_orb_replay`.

## Recorded results — 1 October

| Input | ORB features | Online OK frames | Retained final poses |
| --- | ---: | ---: | ---: |
| Retake `20261001T053033Z-f37549ec` | 1,000 | 4 / 287 | 0 |
| Same retake | 2,000 | 4 / 287 | 0 |
| Earlier lap `20260925T203843Z-3fb5185d` | 2,000 | 5 / 222 | 0 |

All processed every input frame and exited normally. Briefly initialized maps
were reset/discarded. These are failed mapping trials, not successful geometry
validation. A successful positive-control sequence is still needed to exercise
the native final-pose export with real retained maps. Feature count alone did not
resolve these recordings. The second configuration is reproduced with
`--features 2000`; the dashboard default remains 1,000.

The same viewer also has an **Offline partial scan · COLMAP** source. This is a
separate export of the existing strict seed-42 component 3, using
`Mapping.partial_replay`. Its saved seed comparison passed the partial
repeatability screening. The partial export contains 58 camera poses and 3,357
landmarks (at least three observations), grouped into 1,820 coarse display cells.
It preserves the full 287-frame timeline and hides positions for absent views.
The label explicitly distinguishes this offline fragment from ORB-SLAM3 output.
It is still a partial scene with unknown scale and unvalidated physical shape.
