# WALLHACK — current context

Updated **1 October 2026**. Read this before older handoffs.

## Next software direction and repository checkpoint — 1 October

Evan asked for bridge approaches and authorized committing/pushing the current
mapping work. The recommended next offline step is an independent RGB-only replay
against TUM RGB-D depth/trajectory references, then overlapping DA3 windows with
one retained map frame and explicit rejection of unstable alignment. These are
proposed experiments, not completed benchmarks or a working streaming mapper.
[Docs/room-mapping.md](Docs/room-mapping.md) records the sequence and alternatives.
DA3-Streaming's official implementation uses CUDA-specific calls/`faiss-gpu`;
the existing Small model works on MPS, but streaming compatibility is unproven.
OpenVINS on public camera/IMU recordings is the next fusion candidate for the
planned IMU. MASt3R-SLAM needs a supported CUDA setup; stereo/RGB-D plus RTAB-Map
is a hardware alternative. Image-linked landmarks can provide an intermediate
operator summary while metric registration is unavailable.

The commit review also tightened `pose_bridge` timing: only nonnegative integral
nanoseconds are accepted, including NumPy integers; invalid timing withholds
placement. Nonfinite/fractional timestamps and unsigned integer wraparound must
not bypass pose freshness. Raw recordings, model weights and generated viewers
remain under ignored `Saved/`; unrelated `:memory:.ses` is left untouched.
Checkpoint validation: all **68 mapping tests passed**; Python compilation,
JavaScript parsing, shell syntax and `git diff --check` passed. Higher-rate Pi
capture and physical IMU/radar registration are still untested.

## Quick-scan operator experiment completed; IMU planned — 1 October

Evan authorized the 2/5/10-second comparison and operator sketch while away from
the Pi, explicitly reminding us that **the IMU will be available later**. No Pi
connection, capture, deployment, motor action or live radar/IMU was used.

Open `Saved/MappingResearch/quick-scan-20261001/operator-comparison.html`.
The self-contained viewer compares room/stool footage, raw/semantic images,
inferred top-down surface cells and source-linked generic object regions. Empty
cells stay unknown. Contact replay is opt-in and explicitly simulated; tracking
loss withholds current camera/contact placement while keeping the static sketch.
Browser checks passed on desktop/mobile, including playback, duration selection,
map withholding, disabled simulation on bad geometry, and tracking loss.

Six DA3 trials use the same start for each scene, bounded by sensor timestamps:
- Room `20260925T184425Z-78fed556`, start index 169, stride 2.
- Stool `20261001T053033Z-f37549ec`, start index 48, stride 2.
- 2/5/10-second windows select 3/8/15 frames, spanning approximately
  1.3/4.7/9.4 seconds between selected endpoints. Input was recorded at 3 fps.
  Longer-window images are not consumed by short trials.

NVIDIA SegFormer B0 ADE20K labels run locally in a separate research venv;
revision `489d5cd81a0b59fab9b7ea758d3548ebe99677da`, 15 MB safetensors checkpoint,
hashes at `segformer-model/source.json`. No images were uploaded. Model class
confidence is not calibrated probability. The stool is variously labelled chair,
stool and toilet, so the operator display uses generic **object regions** with
candidate labels. The visible room door was not retained as a door landmark;
furniture labels also fail. Qualitative inspection is in `visual-review.json`.

Results: room 2/5 s have insufficient labelled floor for a top-down reference.
Room 10 s yields only a narrow floor candidate from two views and fails the
image-consistency screen (18.41 px median of pair medians at 392 px width).
Its shape is available **only as an unstable diagnostic**, with no camera/contact
placement. Shared-path changes across room windows are large (46% / 85%).
Stool 2/5/10 s produce local floor/object patches and reprojection diagnostics of
2.45 / 2.39 / 2.67 px. But the 2→5 s camera-path comparison changes by 27.8% of
shared-path spread (only three common images), while 5→10 s changes by 6.8%.
These are input-sensitivity diagnostics, not physical accuracy. Even a recognizable
2-second local sketch does not establish reliable localization or a full-room scan.
No single minimum observation duration has been established.

New modules: `Mapping.semantic_infer` (cached labels), `Mapping.operator_sketch`
(floor fit/visible cells/viewer/compact packets), `Mapping.pose_bridge` (explicit
pose-to-observation boundary). `Mapping.da3_trial` adds duration-bounded selection.
Each trial keeps raw depth/poses/semantics, diagnostics, `sketch.json`, compact
`operator-packet.json` and `pose-replay.jsonl`. These are offline, versioned
extensions, not changes to existing live radar/Quest fields. Unstable trials emit
withheld packets with empty geometry and null poses. Arbitrary units cannot be
combined with metre observations. Simulation uses an explicit identity mount and
synthetic fixed point; it checks transforms, not real tracking/radar performance.

**IMU integration is prepared at the pose interface, not implemented as fusion.**
Pose samples include map/clock IDs, timestamp, units, rotation/position, tracking
state, source and `imu_used`. All real trials have `imu_used=false`. A later
calibrated visual-inertial estimator can supply this shape; time synchronization,
actual sensor units/noise/bias and camera-to-IMU mounting remain required.
`Mapping/IMU.md` records the integration order. Tests cover non-identity mount and
rotation, stale/lost/mismatched poses, future fused-pose interface use, floor
support and no-lookahead selection. All **67 mapping tests passed**; exported
packets and simulated placement suppression were checked separately.

## Latest operator goal clarification — away from Pi

Evan is at school without the Pi. Do not make reconnecting it a prerequisite for
design/research or saved-data work. He wants **minimal camera observation time**
for a quick room scan, communicating key landmarks/a rough layout to an operator
alongside the wallhack observations. He asked to explore further AI approaches
and explain localization. Detailed mesh quality remains secondary.

Current recommendation (design proposal, not implemented or benchmarked):
progressively publish a coarse top-down sketch of visible floor/room boundaries,
doorways and large obstacles, retaining source images and marking inferred versus
unknown regions. Use DA3 geometry/pose plus object labels and spatial relations;
a compact object/relationship graph may remain useful before metric scale is
established. Compare 2/5/10-second excerpts of existing recordings and assess
which landmarks/relationships stay correct; duration targets are experimental.
Panoramic layout estimation is an alternative requiring suitable panoramic input
and room-shape assumptions. A fixed reference view can simplify an initial
operator demo; moving-rig radar placement still requires localization, metric
scale, mount alignment and measurement-time poses. Our LD2450 through-wall
performance and moving/tilted-rig fusion remain unvalidated. No new capture or
deployment is requested while he is away.

## DA3 trial completed; higher-FPS capture prepared — 1 October

Evan authorized trying more AI reconstruction and increasing recording FPS.
DA3 Small now runs locally on the M5 Metal GPU using the official network,
preprocessing and output conversion directly (no PyCOLMAP/CUDA renderer imports).
Source commit `3d835ec1a5802d64a8b8b15f817a1ab54809bfe4`; checkpoint revision
`e08cab65ca0ec38e7826075418411ab90cab4da3`. Model download and file hashes are in
`Saved/MappingResearch/da3/model-source.json`. The new isolated venv reuses the
existing research PyTorch 2.14 installation through a `.pth` file. No source
images were uploaded. Six omitted tied-weight aliases are restored by proven
Parameter identity before a strict load; no weights are left randomly initialized.

`Mapping.da3_trial` processed chair retake `20261001T053033Z-f37549ec`:
- `chair-16-c`: original indices 48–108, step 4, 16 images over 20.1 s;
  392×294 output; first successful GPU inference 3.211 s.
- `chair-16-shifted`: indices 52–112, step 4; 0.441 s inference after earlier
  runs. Fifteen views overlap the first trial.
- `chair-24-wide`: indices 48–186, step 6, 24 images over 46.1 s; 0.925 s inference.
  These are model-only timings, not live end-to-end frame rates.

All produced finite positive depth, intrinsics and extrinsics. A recognizable
stool and floor patch are visible in the inferred point cloud, with smearing.
The overlapping batch comparison yields 1.894% path RMSE relative to shared-path
spread after similarity alignment, 2.069° p90 orientation change and 2.498%
median aligned-depth change. **These are input-sensitivity checks, not accuracy.**
Mutual SIFT correspondence reprojection diagnostic: median of pair medians is
4.63 px / 5.84 px / 8.32 px respectively at 392 px width. Longer-section errors
are larger. No metric scale, full room, people registration or continuous tracking
was established. Existing COLMAP and ORB artifacts remain separate and unchanged.

Open `Saved/MappingResearch/da3/comparison.html`: self-contained local viewer,
three input selections, camera/depth comparison, interactive coloured points and
camera path, selected-view toggle, playback and diagnostics. Each trial includes
raw `prediction.npz`, provenance, diagnostics and inferred PLY. Generation:
`Mapping.da3_inspect`. Desktop/mobile browser controls checked, no JS errors.

**FPS change is local only, not deployed or hardware-tested.** The Pi did not
respond at `172.20.10.3`; hostname lookup also failed. Evan was asked whether it
is powered and on the same hotspot. No new recording or remote restart occurred.
Local CLI default is now 12 saved fps (previously 3), with configurable 3/6/12/24
fps in the dashboard and a hard 24 fps recorder limit. The sensor remains requested
at 24 fps and preview capped at 12. Rate changes are blocked while recording;
fixed-deadline sampling with 2 ms maximum arrival tolerance avoids jitter-induced
undersampling. Queue/drop accounting and sensor timestamps remain intact. The
dashboard disables rate control on old Pi code instead of silently claiming it
was applied. Next: reconnect SSH, inspect remote differences, deploy the two
camera modules, then run a bounded 12 fps stationary throughput check before a
moving scan. Shorter exposure/better light remain necessary to address blur.

## Doorway depth estimate — 1 October

On Evan's request, the installed Apple Core ML Depth Anything V2 Small F16 model
processed five evenly spaced images from stationary clip
`20261001T085810Z-f0c68890`. All produced finite 518×392 relative-depth arrays.
The inspected example distinguishes nearer foreground furniture from the farther
door area, but this is model inference with no measured distance or 3D validation.
Warm median inference was 20.75 ms (model-only; not full pipeline throughput).
Across the five static samples, independently normalized display maps differed
from the first by mean absolute 0.0234 on a 0–1 display scale; this is display
variation, not 2.34% physical depth error.

Artifacts: `Saved/MappingResearch/doorway-depth-20261001/` contains the labelled
`relative-depth-example.jpg`, five-frame contact sheet, raw `.npy` predictions,
timing summary and `static-consistency.json`. The relative model cannot supply
metres, camera motion, unseen geometry or a persistent room map from this static
clip. No sparse map was fabricated and no surface fusion was published. Core ML
ran locally with macOS cache access; source images were not uploaded.

## Stationary doorway check completed — 1 October, 08:58–09:08 UTC

Evan is going to sleep and left the powered camera facing a doorway. He authorized
tests possible without moving it. The Pi HTTP camera was live and idle at
172.20.10.3; its SSH control connection expired and batch login failed. Do not
claim fresh temperature/throttling/power readings from this test.

Started one automatically bounded 120-second recording,
`20261001T085810Z-f0c68890`, with the existing 3 fps / auto exposure settings.
`Mapping.stationary_check` downloads and checks its saved frames, then samples
only HTTP camera-status metadata until ten minutes total. It does not change
camera configuration, run radar/motors, or continuously record overnight.
The laptop process uses a temporary `caffeinate -i` assertion that ends with the
test; closing the lid or losing the hotspot can still interrupt monitoring.

Run PID 48877; launcher details in `Saved/MappingResearch/doorway-static-latest.json`.
Results: `Saved/MappingResearch/doorway-static-20261001T085809Z/summary.json`,
`status.jsonl`, `preview.jsonl`; process log is the sibling `.log` file. The
summary is updated automatically. Check its state before reporting completion.
Initial samples showed live capture, increasing preview frame IDs and zero
dropped saved images. The view is a static door/wall scene; this is a capture
reliability and lighting check, not a 3D scan or moving-camera tracking validation.

At 09:00 UTC the clip stopped by its own time limit and was imported/checked:
360/360 images decoded, zero drops, strictly increasing host/sensor timestamps,
119.85 seconds, median 2.998 saved fps, largest gap 0.375 seconds. Median exposure
41.621 ms; median ORB image features 637 (not a tracking result). All 59 initial
status/preview checks succeeded and preview IDs increased. The Pi is ready, no
longer recording. Final summary verified on Evan's follow-up: the check completed
after 600.01 seconds, with 90 successful status samples, zero request errors,
zero camera-not-live samples and zero reported camera errors. All 59 sampled
previews decoded and their frame IDs increased. Monitoring stopped automatically;
the last sampled camera state was ready. This is a ten-minute stationary result,
not overnight endurance, motion-tracking validation or a power/temperature test.

## Built tracking experiment and coarse partial viewer — 1 October

Evan authorized building the proposed next method and inspecting results.
ORB-SLAM3 now compiles and runs on the M5 through a headless C++14 adapter,
upstream commit `4452a3c4ab75b1cde34e5505a36ec3f9edcdc4c4`. Optional build script:
`Build/build_orb_replay.sh`; reproduction and limitations: `Mapping/orb/README.md`.
OpenCV 4.11.0/Eigen 3.4.0 are local research dependencies; Homebrew cmake,
boost and eigen were installed (the build uses pinned local Eigen 3.4.0).

Three real paced replays completed: the retake with 1,000 and 2,000 ORB features,
and the earlier calibrated chair lap with 2,000 features. They briefly tracked
4, 4 and 5 frames respectively, then discarded their small maps. **All three
retained zero final poses/landmarks.** These are unsuccessful mapping results;
this does not establish that low sampling rate alone caused failure. No tracking
thresholds were relaxed. Higher-rate timestamped capture, less blur and a successful
positive-control sequence remain needed before relying on this backend's map export.
The Pi capture settings remain unchanged at 3 fps saved images.

The laptop dashboard now exposes **Test continuous tracking**, **Replay tracking**
and **View coarse partial scan**. New replay routes preserve the original COLMAP
scene. The partial view explicitly exports existing strict seed-42 component 3:
58/287 recovered views, 3,357 landmarks with at least three observations, 1,820
coarse display groups. Its earlier independent build comparison passed partial
repeatability screening (2.31% normalized path disagreement); physical accuracy
remains unvalidated. This is a small offline fragment, not ORB output or a room
floor plan. No walls, free-space classification or people positions were fabricated.

Open: `http://127.0.0.1:8766/map-assets/tracking.html?source=partial&session=20261001T053033Z-f37549ec`.
The result selector switches to ORB trials. Playback pairs saved imagery with
positions, hides unavailable markers, and never joins path lines across gaps.
Desktop/mobile browser checks passed with no JavaScript errors or horizontal
overflow. Tests cover timestamp/calibration conventions, failed-frame hiding,
fragment separation, incomplete-run rejection and read-only recorded-image routes.
Research logs/screenshots: `Saved/MappingResearch/orb-slam3/`; replay data live
under each recording's `tracking/`; selected partial export under `partial/`.
The current local server PID is 48501; log `Saved/MappingSetup/laptop-server-20261001.log`.

## Confirmed use case — 1 October

Evan clarified that the first version should **show a rough room layout with
approximate drone/people positions**. Fine object detail is unnecessary. Prioritize
coherent room structure, camera/drone tracking, and spatially registered live
observations. A chair reconstruction is a diagnostic, not the product milestone.
Quest remains optional. Autonomous obstacle avoidance is outside this first goal.

Lower detail does not resolve the current full-map instability: repeated builds
disagree about camera positions and overall shape. Retain the capture, calibration,
storage and viewer work. The recommended next experiment is continuous camera
tracking with ORB-SLAM3 on the laptop, followed by a coarse room representation;
this backend is now built and trialled as recorded above, **not validated**. ORB's sparse map alone
does not establish walls, free space or person locations. The existing recordings
sample 3 images/second; they permit an initial replay but a proper tracking trial
needs higher-rate timestamped capture and controlled blur. Monocular scale and
camera/radar registration must be established before overlaying metric targets.
The FC IMU is still disconnected; stereo/depth with RTAB-Map is an alternative
if existing-camera trials cannot produce useful layout and tracking.

## Latest chair retake — 1 October

`20261001T053033Z-f37549ec` is saved on the Pi and laptop, labelled
**CHAIR LAP · stationary-chair retake**. Evan reported completion after instructions
to leave the chair still. It contains 287 images over about 96 seconds, zero
drops, no decoding failures, and increasing host/sensor timestamps. Exposure
remained 41.621 ms throughout. The recording includes the laptop at its start/end
and a chair lap in between. The preceding take `20261001T052625Z-8ddcedcb` saved
360 images on the Pi; Evan reported moving the chair as well as the camera.

The existing lens candidate was explicitly applied to the new retake after image
geometry checks, assuming the same physical lens/mount (no change was reported).
The published XFeat result recovered 216/287 views and 22,029 points in the
largest of two components. Its second build recovered 217 views, but the common
camera paths disagree by **88.89% of reference path spread**, with 52.05 degrees
p90 orientation disagreement. The dashboard serves the instability warning.

An isolated stricter comparison produced six/seven fragments. The largest,
62-view fragment includes the laptop portions and still fails repeatability
(8.43% path disagreement). Two chair fragments are repeatable under the existing
screening thresholds: 53/52 views with 1.57% disagreement, and 58/60 views with
2.31%. These are partial fragments, not a continuous chair/room map or measured
accuracy. No strict fragment replaced the full published result; no AI surface
layer was built for this unstable map. Preserve the recording for further
estimator/scene diagnostics before requesting another similar lap.

Artifacts: `Saved/MappingResearch/chair-retake-20261001/` contains capture checks,
a contact sheet, strict models, logs and per-component comparisons. Full models,
calibration provenance and repeatability results are in the session's
`reconstructions/20261001T053323Z-c89c3e7e/`. Final HTTP checks served the model
with its warning and confirmed the Pi camera ready, not recording.

The portable power bank passed a short 14-second stationary recording test
(42 images, zero drops, sampled input 4.97–5.04 V, `get_throttled=0x0`). This
does not establish peak-load capability or runtime; its output current rating
is still unknown. Repeated hotspot outages/restarts followed, with no established
cause. The Pi eventually reappeared at 172.20.10.3 and mDNS resolved again.
Power-test artifacts are under `Saved/MappingResearch/reconnect-20260930/`.

## Reconnected on 30 September

SSH is authenticated through `Saved/MappingSetup/ssh/pi-control`. The Pi is
`172.20.10.3`; `larp-pi.local` failed to resolve on this hotspot. The login helper
now accepts an optional IP while still checking the saved `larp-pi.local` host key:
`bash Build/connect_mapping_pi.sh 172.20.10.3`. The temporary connection still
expires after 20 idle minutes. The laptop dashboard was restarted with
`--pi-http http://172.20.10.3:8766`; status and JPEG preview both passed.

Boot startup is now observed: the installed cron entry launched the sole
mapping-only camera owner at boot, and its bounded automatic recording completed
with 360 images and zero drops. Pi temperature was 36.7 C, `get_throttled=0x0`,
and about 30 GB remained. The startup recording has an old wall-clock identifier
(`20260928T183927Z-0eabcf35`) despite frame monotonic times spanning 15–135 seconds
of the current boot. The clock reports synchronized now; treat startup wall-clock
dates as unreliable until clock synchronization is checked. Original data remain
unchanged. No new operator recording or motor process was started.

Additional saved-image diagnostics confirm the chair-lap break. At 38.02 seconds,
XFeat has 296 verified matches to the previous image but only **28 pose inliers**
against either saved strict fragment (3 px residual), below the strict 60-inlier
gate. Subsequent tested images through 42.69 seconds also fail that gate. SIFT
has no verified links to the preceding 15 images at 38.02–39.02 seconds. These
are diagnostics against final saved models, not an exact replay of incremental
registration or independent accuracy measurements. Preserve the conservative
gates. Next physical input: better light and overlapping translation through
the turn, checking the preview before another scan. More AI surface points will
not restore missing camera-pose support.

Reproduction and both model-seed reports:
`Saved/MappingResearch/reconnect-20260930/diagnose_transition.py` and
`transition-seed-{42,43}.json`. No map was rebuilt or replaced in this session.

## Direction

Build a small drone scout that gives an operator a live view of people and the
space around them, first on a laptop and later in the native Meta Quest HUD.
Hack the North 2026 is over; the project continues.

**The next milestone is room mapping: create a room map, save it, reopen it, and
display live sensor observations against it. Test the existing hardware before
choosing additional sensors.** The comparison, proposed architecture and test
sequence live in [the room-mapping decision record](Docs/room-mapping.md).

**Confirmed preference:** the baseline must work without Quest; Quest can be
used later. A drone-camera recording/reconstruction workflow is now implemented:
[pick-up-and-walk setup](Mapping/README.md). It records on the Pi, reconstructs
after the walk with COLMAP on the laptop CPU, and displays a saved sparse 3D map
in the browser. It is not yet live SLAM or a measured wall/floor map.

**Latest experiment batch:** [measured results and current controls](Docs/mapping-experiments-2026-09-25.md).
The dashboard now has separate AI-inferred surfaces, a live grayscale depth
preview, experimental localization within an existing saved map, and optional
burst/exposure controls. The map is still built after recording; live localization
does not expand it. Depth is inferred from the normal camera, **not thermal**.

**Camera calibration capture completed:** `20260925T202722Z-b884439e` saved
352 images without drops (automatic 120-second stop). Forty distinct full-board
views yielded a lens candidate with 0.251 px training RMS and 0.202 px median
held-out error. Early/late subset fits were also checked. Candidate and diagnostics
are under `Saved/MappingResearch/calibration-20260925T202722Z-b884439e/`.
This is not metric room validation; calibration is not yet the dashboard default.
The OpenCV 5 flat corner-array shape was normalized and covered by a regression test.

**New chair lap:** `20260925T203843Z-3fb5185d` recorded 74 seconds / 222 frames
with zero drops. The lens estimate is explicitly enabled for this recording via
`camera-calibration.json`; it is not a global default. XFeat recovered 222/222
views and 15,453 points in one component. A separate strict AI surface layer has
213,182 points from 160 aligned views. Both are saved and served in the dashboard.
The full-clip repeatability test still changed the camera path substantially
(87.7% of reference path spread after similarity alignment), so the map has an
explicit shape warning. This is not a metric-accuracy measurement. Artifacts:
`Saved/MappingResearch/chair-lap-20260925T203843Z-3fb5185d/`.
Two first-60-second / 180-image builds also differed by 49.2% of path spread;
discarding the desk/person ending did not resolve instability. Retain these
recordings for software investigation before asking Evan for another capture.
The mapper now supports explicit per-session lens candidates with geometry checks,
fixed intrinsics, pixel-center conversion and provenance snapshots. Live localization
accepts OPENCV maps.

**Stability investigation:** isolated same-recording trials are under
`Saved/MappingResearch/stability-20260925/`. Turning off structure-less fallback
reduced the XFeat result to 183–184 views but still gave 30.1% path disagreement.
Strict pose/filter gates recovered 99–100 views with 1.32% disagreement; ordinary
SIFT recovered 96 views with 0.76%. These repeatable fragments cover only the first
~38 seconds. Their 96 common camera poses agree to 3.88% path spread across matchers.
A blurred swing away from the chair at 37–39 seconds is a likely break; the faster
global solver was substantially worse (93.0% disagreement) and was rejected.
These percentages describe repeatability, not independently measured accuracy.
Future **Experimental matching** builds disable structure-less fallback and run
seeds 42/43, saving automatic shape/coverage diagnostics in `quality.json` and the
viewer. No stable fragment silently replaces the saved full map. All 50 mapping
tests pass, including scale/rotation invariance and distorted-path rejection.
LighterGlue was tested with Kornia 0.8.3: 201/201 views but 73.9% disagreement;
strict geometry gave 105/104 views and 15.0%. Matching took ~120 seconds, so it
was not promoted. Six settings / twelve new builds are now summarized at
`http://127.0.0.1:8766/map-assets/stability-20260925.html`, linked from the dashboard.
Next input should improve the blurry transition and visual overlap; more dense
AI points will not repair the camera path. No new recording was started.

## Which information to trust

The user supplied a separate **WALLHACK handoff dated 25 Sep 2026** describing
`scripts/infer_stream.py`, `ground/bridge.py`, `ground/visual_odometry.py`,
`map.html` and Hailo inference. **Those files are absent from this checkout.**
Its intent and reported physical hardware inform this context; its software
implementation and test claims must not be attributed to this repository.

This checkout instead contains `SensorRig/`, `GroundStation/`, and the Unreal
project. The older [Pi handoff](SensorRig/HANDOFF.md) records the 19 Sep bench
setup; [GroundStation/README.md](GroundStation/README.md) describes the later
stationary camera/radar association and native Quest integration in this tree.
Use source code for implemented behavior and recorded hardware evidence for
physical validation. Reconcile any other checkout with this one before porting.

The pasted handoff also reports unrelated proprietary `Backend/`, `Frontend/`
and `documentation/` code was purged elsewhere. Do not restore those directories
while consolidating project copies; that purge/history was not re-verified here.

## Existing system and hardware

| Component | Evidence and current constraint |
| --- | --- |
| Raspberry Pi 5 + IMX219 CSI camera | Recorded working station: 640×480, 24 fps capture request, 180° image rotation. Actual lens intrinsics/distortion need calibration. |
| Person detector | This checkout uses YOLOX nano through ONNX Runtime CPU. Hailo-10H was reported in the pasted handoff; current fitment/runtime are unverified and mapping must not depend on it. |
| HLK-LD2450 | Existing UART parser, mounting controls and live targets. Saved `invert_x: true` corrects an observed reversal. Preserve it until a physical measurement warrants changing it. |
| Flight controller / IMU | Evan clarified on 25 Sep that the Pi is **not connected to the FC**; his friend handled the hardware. No FC USB serial device currently enumerates on Mac/Pi. Old `SensorRig/Fusion/imu_viz.py` expects MSP on Windows COM4 and also sends motor commands. A separate read-only `Mapping.imu_logger` is prepared and unit-tested, but physical FC acquisition is untested. Exact board, firmware, IMU, timing and units remain unknown. See [connection and logging notes](Mapping/IMU.md). |
| Drone | User-reported 5-inch quad, Pi cooler, forward M12-lens camera, 1800 mAh flight battery. USB-C bench power; flight BEC, payload and vibration performance remain to be checked. |
| Laptop | Confirmed Apple M5, 24 GB RAM. Hosts recording/replay, dashboard and CPU reconstruction; Core ML inferred depth also tested. Pi mapping performance is unmeasured. |
| Meta Quest | Native room-scene and sensor-people code exists. Headset model, compatible SDK/toolchain and physical acceptance need verification. Room export to the laptop is not implemented. |

Recorded Pi identity: `larp-pi`, user `evanl1307`, usually
`larp-pi.local` / `172.20.10.3`; addresses can change. Recorded checkout:
`/home/evanl1307/HTN2026/Sharingan`. **Connected and deployed on 25 Sep 2026** after
the user authenticated with `Build/connect_mapping_pi.sh`. That creates a temporary
SSH control connection with a 20-minute idle expiry, without storing a password or
installing a key. The Pi checkout was `4a6bc8d` before the capture-only update;
previous files are backed up in `Saved/MappingSetup/20260925T182617Z-2203` on the Pi.
IMX219, Picamera2, OpenCV 4.10 and the sibling Python venv were verified directly.
The camera-only process is running and reachable through the laptop dashboard.

**Preview latency fixed:** live preview uses a separate 12 fps JPEG encoder and
a continuous MJPEG stream through the laptop. Recording remains 3 fps at JPEG
quality 95; the lighter preview uses quality 75 and one replaceable pending frame.
The browser no longer waits for its 1.8-second status poll to refresh an image.
HTTP `.local` Pi addresses resolve to IPv4 at laptop startup to avoid the observed
IPv6 connection fallback delay. Restart the laptop app if the Pi's address changes.
Measured 70 frames through the laptop: 11.99 fps, 82 ms median inter-frame interval,
104 ms first-frame request time. These are transport measurements, not measured
camera-to-screen motion latency. Evidence: `Saved/MappingSetup/verification/preview-latency.json`.

User `evanl1307` now has one `@reboot` cron entry invoking
`Build/run_mapping_pi_background.sh`. Cron is active; the wrapper uses `flock` to
prevent duplicate wrapper launches and logs to `Saved/MappingCapture/service.log`.
It records one bounded two-minute session at camera startup, then remains ready
for browser-controlled walks. **Boot execution has not been tested by rebooting.**
The alternative system-wide systemd unit is not installed. No FC/motor program
was launched.

```text
Pi: single camera owner -> person detections --\
Pi: independent LD2450 reader ----------------> laptop relay/dashboard -> Quest
Quest: MRUK room scene -> existing native navigation code
                        -> proposed saved room-map export / laptop view
Pi: clean sampled images + capture metadata -> saved walk -> laptop COLMAP -> /map
FC IMU integration and Quest map import/alignment -> later experiments
```

The current laptop/native sensor path assumes a **stationary, level rig**.
Camera/radar association requires measured alignment and explicit enablement.
Radar-only targets remain unclassified; camera range remains an estimate.
Quest controller placement supplies a session reference, not automatic drone
tracking. The native room-scene code is not yet a shared persistent laptop map.

## What the new work must preserve

- One camera owner and one radar UART owner. Mapping consumes a frame branch;
  it must not open the CSI camera again or use annotated detector images.
- Independent observation expiry: camera 750 ms, radar 500 ms; the legacy rig
  pose TTL is 1 s. The pasted handoff's universal 1 s TTL does not describe this tree.
- Current radar mount correction and metre-based right/forward coordinates.
  Moving-rig compensation requires a pose at measurement time and new tests.
- Existing packet fields and stationary Quest mode. Introduce mapping through
  a separate versioned extension, without silently changing coordinate meaning.
- Static room geometry persists; expired people and lost rig poses do not.
- No claim that the LD2450 maps walls, reliably sees through drywall, or detects
  motionless/unconscious people. Physical coverage remains an experiment.
- Phone pose and runtime AprilTag localization are not the selected design.
  An offline printed calibration board does not require room-mounted markers.

## Next work, in order

1. **Recover reliable camera tracking through the difficult transition.** Use
   the new repeat-build harness on existing images. Compare matcher quality and
   temporal overlap rather than optimizing for camera/point count. Keep inferred
   depth downstream of reliable poses; it cannot establish missing camera motion.
2. **Then test capture feedback.** The lens candidate and new chair lap are
   already captured. Add light and preserve overlapping views through turns.
   Short exposure and sharpest-burst controls exist: 19.995/9.992 ms were verified,
   but looked dark in this room. Auto exposure/uniform sampling remain selected.
   Test burst selection with motion/overlap feedback before changing the default;
   simply thinning an old 97-frame recording to 50 reduced recovery to 22 views.
3. **Validate shape and move toward live tracking.** Measure independent chair
   dimensions once a stable full reconstruction exists; the rough 40 cm seat
   diameter is recorded but unapplied. ORB-SLAM3 remains a future comparison.
   Full-rate timestamped capture is still needed for a serious live/VIO trial.
4. **Assess the existing IMU.** Connect the FC data path first (USB to Pi is the
   proposed first bench test); use the prepared [read-only logger](Mapping/IMU.md), measure
   sample timing, then try calibrated visual-inertial estimation if the data
   support it. Do not use `imu_viz.py` as the mapping logger: it also sends motor
   commands and its current readings are not a synchronized VIO dataset.
5. **Choose from evidence.** Continue camera/IMU if it meets the agreed goals;
   borrow a stereo/depth camera if usable room geometry is the limiting factor.
   A BNO085 purchase is no longer the automatic next step.
6. **Integrate moving-rig pose, then flight.** Add map registration, capture-time
   transforms and loss handling; evaluate power, vibration and tilted-radar
   limitations before relying on the map during flight. Autonomy follows these tests.

Quest scene export can be tried after this baseline. Combining Quest/drone maps
requires explicit scale/rotation/translation alignment and independent landmark
checks. It is optional, and not implemented in the current mapping viewer.

**Lightweight depth experiment:** Apple's Core ML Depth Anything V2 Small runs
at 21.3 ms median warm inference on this M5 (518×392 input, ~50 MB model). Saved
examples are under `Saved/MappingResearch/depth-second-walk/`. A live preview and
separate experimental fused layer are now implemented. The chair map currently
has 55,726 inferred points from 74 aligned views using stricter cross-view checks.
Visible smearing remains; physical shape and scale are unvalidated. The first
walk benefited much less (55/224 alignable views). Keep inferred surfaces separate
from triangulated observations and unknown space. The user estimates the chair
seat diameter at roughly 40 cm; stored as a provisional reference, not applied scale.
Research code, pinned model provenance and reproduction: [Mapping/RESEARCH.md](Mapping/RESEARCH.md).

**Processing split:** Pi captures/timestamps and optionally selects sharper frames,
buffers originals and transmits selected images. Laptop estimates camera motion,
triangulates geometry, predicts relative depth and fuses only cross-view-consistent
surfaces. Reconstruction is still after recording. Experimental live localization
matches the camera to an existing map and expires poses after 750 ms; it is not
live SLAM. XFeat was exported to a 2.75 MB ONNX model and benchmarked on the Pi:
85.6 ms/image at two threads; four threads were slower and the Pi recorded a past
undervoltage/throttling event. It is not enabled as a continuous onboard worker.
Feature-only packets
are not automatically smaller than images: the chair scan's median JPEG is
54,384 bytes, versus 262,144 bytes for 1,024 uncompressed 64-D float32 descriptors
before keypoint metadata. Retain image access for depth, color and reprocessing.
The next physical input is a varied checkerboard recording. A display/print board
and calibration CLI are ready; synthetic recovery passed, but no real calibration
has been performed. Repeated sparse reconstruction changed shape substantially;
do not promote faster optimization solely on camera count. See the
[experiment ledger](Docs/mapping-experiments-2026-09-25.md).

## Where things live

- Project state: this file. Options, references and acceptance criteria:
  [Docs/room-mapping.md](Docs/room-mapping.md).
- Existing live sensor dashboard: `http://<laptop>:8766/`; see the
  [runbook](GroundStation/README.md) for starting it.
- **Implemented:** `http://localhost:8766/map`, standalone via
  `bash Build/start_mapping_laptop.sh` or as part of the existing relay. Persistent
  laptop sessions: `Saved/Mapping/<session_id>/`; Pi: `Saved/MappingCapture/`.
  No cloud/Meta account is needed. The standalone app defaults to localhost.
- Validation: 46 ground-station, 43 Pi and 33 mapping tests pass locally; the older
  checkout's 39 Pi tests also passed on the actual Pi after deployment. Real
  COLMAP execution recovered 16/16 cameras and 1,887 points on a labelled synthetic
  scene. Physical setup capture `20260925T182625Z-29f89c70` saved 165 decodable
  640×480 JPEGs with sensor timestamps and no dropped frames. Its downloaded archive
  and report are under `Saved/MappingSetup/verification/` on this laptop. A real
  room walk now has a learned sparse result (above); useful room coverage,
  metric accuracy, flight and reboot startup remain unverified.
  After the preview update, verification recording `20260925T183833Z-0113e555`
  saved another 51 decodable images with sensor metadata and zero drops.
  The mapper now rejects components with too little geometry before ranking them;
  for walks up to 400 images it automatically tries exhaustive matching if the
  initial usable component covers less than half the images. Coverage below 80%
  is labelled partial, a display heuristic rather than an accuracy acceptance test.
