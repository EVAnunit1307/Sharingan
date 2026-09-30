# WALLHACK — current context

Updated **25 September 2026**. Read this before older handoffs.

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
