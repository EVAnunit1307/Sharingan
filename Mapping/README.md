# Pick up the drone and record a room

## Latest offline model comparison — 3 October

After unloading Ollama, DA3 Large completed the same 24-image wide-view test at
504 resolution: 0.86 vs Base's 1.74 px adjacent image discrepancy, with warm model
time about 7.4 vs 2.4 s and a sampled Metal peak of 10.12 GiB. This is one
same-input consistency check, not physical accuracy. Open the
[Large comparison](examples/pose-depth-20261003/large.html) or its
[reproduction notes](examples/pose-depth-20261003/README.md).

The [Small/Base and pose-conditioning experiment](examples/pose-depth-20261003/README.md)
includes both fixed-image comparisons, source frames, selected tracked poses,
renders and reproduction commands. Base runs on the M5 and improves the first
wider-view image check. Correcting DA3's centered-camera assumption requires a
substantial crop with this lens calibration; guidance gives mixed results.
No production mapping default changed. Neither run establishes physical accuracy.

## Teammate demo: open or rebuild the room draft

The [committed example](examples/room-draft-20261002/README.md) contains an offline
interactive viewer, four PNG renders, 72 original selected images and pinned
reproduction instructions. It includes both wide-room hypotheses, the brighter
sofa/floor pass and the couch return test. Start with `python3 -m Mapping.room_demo verify`, or open the
example's `index.html` directly. No Pi is needed to view or rebuild these AI drafts.
The views have unverified alignment and arbitrary scale; they are not navigation maps.

## Active mapping/CV handoff plan

Follow [the staged plan](../Docs/mapping-cv-handoff.md): camera quality,
continuous localization, a repeatable rough map, concurrent person detection,
then software handoff. Stage 1 has a well-lit controlled baseline; Stage 2 loop
and repeatability checks are active. Herman handles flight and the IMU
connection; later sensor fusion still needs an agreed interface and calibration.

## First retained Pi tracking pass — 2 October

**Latest couch return test:** `20261002T213938Z-e3ead809` retains 614/658 poses
(93.3%) in one map, continuously for 51.4 seconds after 3.7 seconds initializing.
All images decode; zero reported queue drops. This passes the single-run coverage
target. Evan confirms an approximate return; exact pose, physical drift and
repeatability still need checking.
Review: `Saved/MappingResearch/capture-quality-20261002/couch-loop-2137Z/review.html`.

`Saved/MappingResearch/capture-quality-20261002/lit-motion-203958Z/review.html`
shows the brighter sofa/floor pass and links to its camera path and AI draft.
331 readable frames at 12 fps, fixed 20 ms / 4× gain; ORB retains 213 consecutive
poses in one map for 17.7 seconds after a 9.9-second initialization. All-frame
coverage is 64.4%; repeatability, physical accuracy and metric scale are pending.

The capture dropdown now offers **Well-lit room · 20 ms**: uniform selection,
20,000 us, gain 4. Select **12 fps** to reproduce this test, reload the page to
pick up the new option, and apply while idle. Check floor/furniture brightness
first; it is a profile tested in this lighting, not a universal default.
**Standard capture** restores auto exposure, which was restored after the test.
The AI scene draft uses independent predicted poses; it is not registered to the
ORB trajectory and does not establish contact placement.

## AI room draft — 2 October

Evan requested a fuller tentative picture even when estimates are uncertain.
Open `Saved/MappingResearch/ai-room-draft-20261002/room-draft.html` for two
wider-room hypotheses and the latest sofa close-up. The viewer includes weaker
depth estimates in faint amber, optional semantic colours, tentative object
labels and source-image inspection. Each hypothesis keeps its own frame. Model
confidence ranks are not calibrated probabilities of correctness.

`Mapping.ai_room_draft` reads completed predictions and optional semantic labels,
then writes only a new HTML viewer and JSON manifest. It does not write operator
maps, pose packets or source trials. Existing tracking/placement gates are
unchanged. A model may invent or distort visible geometry; unseen room boundaries
remain unknown. This is an additional operator-context experiment, not a passed
continuous-localization or room-map stage.

```sh
.venv/bin/python -m Mapping.ai_room_draft \
  --trials Saved/MappingResearch/ai-room-draft-20261002/room-camera \
    Saved/MappingResearch/ai-room-draft-20261002/room-rays-cpu \
    Saved/MappingResearch/capture-quality-20261002/motion-194831Z/dense-0108 \
  --labels 'Room sweep' 'Room sweep · alternate estimate' 'Sofa close-up' \
  --output Saved/MappingResearch/ai-room-draft-20261002/new-draft.html
```

The wider trials use 24 images at rows 192..744, stride 24, of recording
`20261002T185247Z-037db03f` (46.16 s between endpoints), at 504×378.
Both have zero adjacent pairs with enough image matches; their alignment is
unverified. The alternate `Mapping.da3_trial --ray-pose` uses the official ray
head. Its MPS SVD call failed in Metal; `--device cpu --ray-pose` completed
(16.49 s model call, versus 1.43 s for camera-decoder MPS). This one comparison
does not establish an accuracy or speed improvement. Failure and successful-run
logs are preserved; no upstream model code was edited. Object labels use the
existing local SegFormer model/cache. All image inference stays on the Mac.

Checks: `python -m unittest Mapping.tests.test_ai_room_draft Mapping.tests.test_da3`;
browser checks cover uncertainty filtering, source selection, semantic overlays,
single-view inspection, playback and mobile layout.

## New room walk — 2 October

`Saved/MappingResearch/room-walk-20261002/review.html` shows the new 12 fps room
recording, short-window results and one recovered local 3D fragment. All 1,437
saved images decoded; 0/21 broad replay windows passed screening. Denser 24-view
input recovered a 1.92-second sofa/table/wall fragment, but the next overlapping
window disagreed, so continuous mapping remains unvalidated. The 10-second
top-down sketch is labelled unstable and withholds camera/contact placement.
Original images, raw predictions, selection plans and diagnostics are retained.

## M5 capacity and overlapping-window experiment — 1 October

Open `Saved/MappingResearch/mac-m5-20261001/comparison.html`. The M5/24 GB Mac
completed four bounded DA3 Small profiles and 23 window batches. A warm 24-image
call at 504×378 took about 0.97 s with a sampled 3.17 GiB Metal driver peak;
model-call timing excludes the rest of the pipeline. The stool fragment grows
through three accepted windows (32 poses); room-wide continuity is unresolved.

New tools: `Mapping.reference_benchmark` imports public TUM RGB separately from
reference poses/depth and evaluates predictions after inference;
`Mapping.window_replay` screens overlapping windows before extending a map;
`Mapping.mac_report` renders the experiment. `Mapping.da3_trial` now records
resource/timing data, supports `--repeats 1–3`, and caps the MPS allocator by
default. These are local experiments, not upstream DA3-Streaming or live SLAM.
See [measurements, limitations and reproduction](../Docs/mac-mapping-2026-10-01.md).

## Quick-scan operator sketch — 1 October

Open `Saved/MappingResearch/quick-scan-20261001/operator-comparison.html` for the
2/5/10-second prefix experiment. It uses existing room-sweep and stool recordings,
DA3 geometry/poses, and local NVIDIA SegFormer B0 ADE20K semantic labels. No Pi,
live radar, image upload or IMU was used. Small-window source selection follows
sensor timestamps at stride 2: 3/8/15 images span about 1.3/4.7/9.4 seconds inside
the requested 2/5/10-second windows. Short trials never consume later images.

The sketch projects confidence-filtered visible surfaces into an inferred floor
frame. It does not fill an unseen room polygon or label gaps as free space.
Objects are generic regions because the model confuses labels, including stool,
chair and toilet. A floor supported by multiple views and a reprojection screen
are required for provisional camera/contact placement. The room's 2/5-second
windows have too little floor evidence; its 10-second diagnostic is unstable.
The stool has usable local surface patches, but short-window camera poses remain
sensitive to additional input. No minimum duration for an accurate full room has
been established.

The viewer pairs raw/labelled images with a top-down cell sketch, source-linked
regions, all duration results, and opt-in **simulated** contact replay. Lost
tracking hides current camera/contact placement while preserving static geometry.
Each trial exports `sketch.json` (research), a compact `operator-packet.json`
(`wallhack.operator_sketch.v1`) and timestamped `pose-replay.jsonl`. These are new
offline packet examples, not changes to the current live radar/Quest protocol.
Unstable maps emit withheld packets with no surface cells/landmarks or usable
poses. All maps have distinct IDs and unknown scale.

Reproduction: `Mapping.da3_trial --duration-seconds 2 --stride 2` (also 5/10),
then `Mapping.semantic_infer`, then `Mapping.operator_sketch`. All accept `--help`.
Use the installed `Saved/MappingResearch/quick-scan-20261001/semantic-venv` for
semantic inference; it reuses the research Torch install and isolates Transformers.
Model revision/hash: `segformer-model/source.json` within that research directory.
DA3 generation uses `Saved/MappingResearch/da3/venv`. Source recordings and start
indices are recorded in each trial's `summary.json`; raw model arrays are retained.
Local cache entries contain only image/model-keyed predictions. Browser and test
artifacts are saved in the same research folder.

**IMU remains planned.** `Mapping.pose_bridge` separates map placement from the
pose producer, so a calibrated visual-inertial estimator can replace recorded
visual poses. No inertial fusion is implemented yet. See [IMU integration](IMU.md).

## AI geometry trial and recording rate — 1 October

The latest experiment runs **Depth Anything 3 Small** on saved images, estimating
depth and camera poses jointly. Open `Saved/MappingResearch/da3/comparison.html`
for three local runs of the chair retake. The short run produces a recognizable
stool/floor patch; longer coverage has larger correspondence errors. This is
inferred geometry with unknown scale, not a validated room map. Raw predictions,
checkpoint hashes, source commit, logs and diagnostics are beside the viewer.

The installed research environment is `Saved/MappingResearch/da3/venv`; it reuses
PyTorch from `Saved/MappingResearch/venv` and adds torchvision, einops, addict,
omegaconf, safetensors and imageio. Keep PyCOLMAP out of this process. The runner
uses the upstream network and processors directly to avoid unused CUDA/export
dependencies. The repository checkout is pinned at
`3d835ec1a5802d64a8b8b15f817a1ab54809bfe4`; model revision and hashes are recorded in
`Saved/MappingResearch/da3/model-source.json`. Reproduce with a **new** output path:

```sh
Saved/MappingResearch/da3/venv/bin/python -m Mapping.da3_trial \
  --source Saved/MappingResearch/da3/source --model Saved/MappingResearch/da3/model \
  --session Saved/Mapping/20261001T053033Z-f37549ec \
  --start-frame 48 --stride 4 --frames 16 --resolution 392 \
  --output Saved/MappingResearch/da3/new-trial

Saved/MappingResearch/da3/venv/bin/python -m Mapping.da3_inspect \
  --trials Saved/MappingResearch/da3/chair-16-c \
    Saved/MappingResearch/da3/chair-16-shifted Saved/MappingResearch/da3/chair-24-wide \
  --output Saved/MappingResearch/da3/comparison.html
```

Metal requires macOS GPU access. `--device cpu` is explicit fallback; the ray-head
experiment above exercises it. Batch inference timings exclude loading, image I/O and the viewer.
The comparison aligns shared camera positions and measures input sensitivity;
it does not establish physical accuracy. The PLY is confidence-filtered inferred
depth, not triangulated landmarks or verified free space.

**Recording FPS was deployed and checked on 2 October.** Camera CLI default:
12 saved fps. The dashboard can request 3, 6, 12 or 24 fps between recordings.
The 12 fps stationary check saved 152 decodable images over 12.586 seconds,
measured 11.997 fps, with zero reported drops and increasing sensor timestamps.
Evidence: `Saved/MappingSetup/fps12-20261002/stationary-12fps.json`.
The sensor target remains 24 fps; preview stays at up to 12 fps. Rate control
stays disabled for old Pi software. Higher FPS alone does not shorten exposure:
the check still used 41.621 ms auto exposure. 24 saved fps has not been verified.

## Stationary camera check

For an unattended **stationary camera check**, `Mapping.stationary_check` uses
the existing Pi HTTP camera owner: one recording bounded by the Pi's existing
maximum of 120 seconds, followed by status-only monitoring (default ten minutes
total). It saves a small preview sample, imports the clip and checks decoding,
timestamps, drops and brightness. It leaves exposure/capture settings unchanged.
The camera must already be live and idle; temperature/power are not measured.
Use a new output directory for each run. Example:

```sh
.venv/bin/python -m Mapping.stationary_check --pi-http http://172.20.10.3:8766 \
  --output Saved/MappingResearch/doorway-check-new --monitor-seconds 600
```

This runs on the laptop, so it needs that laptop awake and the hotspot available.
The Pi recording stops independently if the laptop disconnects. Stationary frames
can check capture reliability but cannot validate motion-based room geometry.

This baseline works **without Quest**. The laptop hosts the app and computes the
map; the browser renders it. The Pi saves the walk locally. Quest can later add
room geometry/reference data after coordinate alignment.

| Job | Application / location |
| --- | --- |
| Capture clean images | Existing Pi camera process, mapping-only mode |
| Start/stop, saved walks, 3D viewer | WALLHACK browser page at `http://localhost:8766/map` on the laptop |
| Reconstruct camera path + sparse points | COLMAP through PyCOLMAP 4.2.0, on the laptop CPU |
| Pi recordings | `<Pi repo>/Saved/MappingCapture/<session_id>/` |
| Downloaded images + built maps | `<laptop repo>/Saved/Mapping/<session_id>/` |
| Optional external inspection | COLMAP for its native saved model; a PLY-capable viewer for `points.ply` |

No cloud deployment, Meta login, Unreal build or new sensor is required. This
version **builds after a walk**. Optional live localization can estimate the camera
pose within an existing saved map; it does not build new geometry while moving.
It does not fuse an IMU, import Quest geometry, generate a validated wall mesh or
overlay moving-rig people. Monocular reconstruction has unknown
metric scale; units/orientation in the viewer are explicitly uncalibrated.

The existing FC IMU is a separate connection. A read-only logger is prepared;
the Pi currently has no confirmed FC data link. See [IMU connection and logging](IMU.md).

**1 October: rough-map experiment.** An optional ORB-SLAM3 replay backend is built
on this Mac. Calibrated recordings have **Test continuous tracking** and **Replay
tracking** controls. Current 3 fps chair recordings briefly initialized but retained
no ORB maps. **View coarse partial scan** shows the separately labelled 58-view
offline reconstruction fragment with saved-camera playback. No full room layout
or people positions are established. See [reproduction and results](orb/README.md).

The first real checkerboard recording is now saved as `CAMERA CALIBRATION`
(`20260925T202722Z-b884439e`): 352 images, 40 distinct usable board views.
Its lens candidate passes basic held-out checks. Reconstruction now supports it
as an explicit per-recording experiment; it is enabled for `CHAIR LAP`
(`20260925T203843Z-3fb5185d`). Existing maps remain unchanged. See the calibration
follow-up in the [experiment ledger](../Docs/mapping-experiments-2026-09-25.md).

To opt another compatible recording into this experiment, copy the reviewed
candidate JSON to that recording's `camera-calibration.json` before building.
The mapper checks image dimensions, rotation, source and pixel convention. It
holds intrinsics fixed, reverifies learned correspondences with the new camera
model, and saves a calibration snapshot and source hash with each reconstruction.
Changing the physical lens or mount requires a new calibration; matching metadata
alone cannot detect those changes. The model page labels the applied lens estimate.
Map scale and physical shape remain unvalidated. Both standard and learned builds
support this option, and live localization accepts the resulting OPENCV camera model.

## Current setup — 25 September 2026

**Installed and running on this Mac and `larp-pi.local`.** Open
`http://127.0.0.1:8766/map`; the camera is live and ready for a new recording.
The connection-check recording contains 165 images; it is not a room scan.
All images decoded, and the Pi reported zero dropped frames.

**First room walk:** `20260925T184425Z-78fed556` saved 236 images without drops.
Standard reconstruction recovered 56 views and 245 sparse points; the optional
XFeat matcher recovered **224 views and 7,865 points** from the same images.
**Second walk:** `20260925T185819Z-14c01c84` saved 97 images without drops.
XFeat recovered **all 97 views and 9,149 points in 34.4 seconds**, up from the
standard result of 49 views and 1,296 points. These learned models are saved and
labelled experimental. Their physical shape and metric scale remain unvalidated.

Use **Experimental matching** beside a saved recording to try XFeat on this Mac.
It runs locally in the installed research environment and preserves prior map
revisions. It may improve recovery but does not guarantee a faster build.
New learned builds also reconstruct a second time with a different seed, reject
structure-less fallback registration, and display a repeatability/coverage warning.
The check adds processing time and may reveal disconnected fragments. Passing
means only that two builds agree; physical shape and metric scale remain unvalidated.
See [research setup and reproduction](RESEARCH.md) and the
[measured comparison / next improvements](../Docs/mapping-quality-strategy.md).
Core ML depth is now available as a **live grayscale preview** and a separate
**AI-inferred surface layer** checked across views. It is not thermal imagery.
The chair layer contains 55,726 points under the stricter preset; shape and scale
remain unvalidated and visible smearing remains. Burst/exposure controls are
deployed as optional capture experiments; auto exposure and standard sampling
were restored after testing. See the [complete experiment ledger](../Docs/mapping-experiments-2026-09-25.md).
For now, add light, move sideways slowly around textured furniture and pause
after each step: median exposure is still 41.6 ms.

The dashboard preview requests complete JPEG frames, independently of the 12 fps
recording rate. It downloads and decodes each frame before showing it, with one
request in flight and a maximum request rate of 10 fps; network/decode time can
lower that rate. This replaced the browser's MJPEG display on 2 October after it
showed a partial strip/blank image despite complete Pi JPEGs. The stream endpoint
remains available for other clients. Its earlier 11.99 fps transport measurement
does not measure the new browser preview or camera-to-screen latency.
Lower preview JPEG quality reduces traffic without reducing saved-image quality.
Missing, truncated or delayed frames are withheld. Displayed frames expire within
one second using upstream age plus the full request/decode duration as a
conservative bound. Preview pauses in a hidden tab, aborts the current request,
and resumes when visible. A late response cannot restore a paused image.

Browser regression: `Mapping/tests/check_preview.cjs` uses Playwright with a local
test server and generated JPEGs; no Pi is needed. Run with Playwright installed,
or set `PLAYWRIGHT_MODULE` to its module path and `CHROME_BIN` to a browser binary.
It covers partial downloads, complete image contents, expiry, background tabs,
late responses, stale metadata, truncated JPEGs and recovery.

The Pi starts capture through the user-owned cron entry described below. For this
session, no more terminal setup is needed. If the laptop app is stopped later,
restart it with `bash Build/start_mapping_laptop.sh`. The following installation
instructions are for rebuilding the setup on another checkout.

## One-time setup / reinstall

On the laptop, from this repository:

```sh
bash Build/start_mapping_laptop.sh
```

The launcher creates `.venv` if necessary, installs laptop mapping dependencies
if absent, then serves the page. On Windows, use Python 3.12+ and the equivalent:

```powershell
python -m venv .venv
.venv\Scripts\python -m pip install -r Mapping/requirements.txt
.venv\Scripts\python -m Mapping.server
```

Use `--pi-http http://<actual-pi-address>:8766` if `larp-pi.local` does not resolve.
For HTTP `.local` addresses, the app selects the IPv4 address at startup, avoiding
the measured delay trying an IPv6 address the Pi HTTP server does not listen on.
Restart the laptop app after the Pi moves networks or changes address.
The default laptop binding is localhost. Add `--host 0.0.0.0` only when other
devices on the test LAN should open `http://<laptop-ip>:8766/map`.
The existing `GroundStation.server` also serves `/map`; run one laptop server on
8766 at a time. The standalone mapping launcher does not start the stationary
people relay, which is appropriate while carrying the drone.

**The Pi needs the updated capture files.** If using this full checkout, stop
the known existing camera dashboard, then from the Pi repository root run:

```sh
bash Build/start_mapping_pi.sh
```

It uses the recorded sibling `../.venv` first, then `.venv`. Existing Pi camera,
OpenCV and Flask dependencies are sufficient; do not install PyCOLMAP on the Pi.
Keep the camera at its current mount/rotation for the first walk. Run only one
camera process. The launcher uses `--mapping-only`, so inference, radar and Quest
are not needed. For an existing combined station instead, append `--mapping` to
its normal command to branch the clean images from its camera owner.

A prepared Pi update bundle contains the two changed capture modules and launch
files plus a backup-making installer. It is built locally under
`Saved/MappingSetup/wallhack-mapping-pi.tar.gz`. Copy it to the Pi using your normal
transfer method; for example, **run on the laptop yourself**:

```sh
scp Saved/MappingSetup/wallhack-mapping-pi.tar.gz evanl1307@larp-pi.local:~/
```

Then on the Pi (stop its existing dashboard first):

```sh
mkdir -p ~/wallhack-mapping-setup
tar -xzf ~/wallhack-mapping-pi.tar.gz -C ~/wallhack-mapping-setup
bash ~/wallhack-mapping-setup/install-pi.sh /home/evanl1307/HTN2026/Sharingan
cd /home/evanl1307/HTN2026/Sharingan
bash Build/start_mapping_pi.sh
```

The installer backs up existing versions under `Saved/MappingSetup/`; it does
not kill processes, change firmware, install packages or enable a service.
Deployment completed after the operator ran `bash Build/connect_mapping_pi.sh`
and authenticated in their own terminal. That helper establishes a temporary SSH
control connection (20-minute idle expiry), without storing a password or
installing a new key. It can be used again for future setup work if needed.
The deployed files were backed up under
`Saved/MappingSetup/20260925T182617Z-2203` on the Pi.

## Each walk

1. Remove the props. Keep the Pi powered continuously while carrying the drone;
   use the existing bench supply/tether or a verified portable Pi supply.
2. Start the laptop launcher if needed. The Pi camera starts at boot; do not
   launch a second camera process while it is already running.
   Open `http://localhost:8766/map`; wait for **Camera live**.
3. Press **Start recording**. Carry the camera slowly around a well-lit room for
   60–120 seconds, translating sideways/forward with overlapping views. Start
   with an empty room and textured furniture/walls. Return toward the start.
4. Press **Stop & save**. A recording also stops automatically at 120 seconds.
   A camera reconnect closes that walk instead of mixing camera generations.
5. Press **Build 3D map** on the saved walk. This copies the images to the laptop
   and runs feature matching/reconstruction; it may take several minutes.
   For walks up to 400 images, if fewer than half connect, it automatically tries
   matching across the full walk to connect revisited views.
6. The largest usable reconstructed component opens automatically. Drag to orbit,
   scroll to zoom, or use **Fit view**. Compare registered cameras against total
   images; a fragmented/small reconstruction is a failed or partial baseline.
   Below 80% image coverage, the viewer explicitly labels the result partial.
   This coverage label does not establish geometric accuracy.
7. Reopen it later with **View map**, even with the Pi offline. Preserve the
   original recording for the next estimator/calibration experiment.

Optional experiments in the same page:

- **Add AI surfaces:** estimate relative depth, align it to the sparse map and
  require agreement from at least three additional views. Use **Show** to switch
  layers. This remains inferred geometry, not a measured mesh.
- **Start depth preview:** compare the normal camera to grayscale estimated depth.
  Bright means nearer; there are no measured metres or temperatures.
- **Locate camera in this map:** point at the previously scanned area. An orange
  camera estimate appears when enough landmarks match, then disappears on lost
  or stale tracking. No new room geometry or flight control is produced.
- **Capture experiment:** choose sharper-frame selection and optional 20/10 ms
  exposures between recordings. Check brightness; Standard restores auto exposure.
  Settings reset to standard at camera restart.
- For lens calibration, open [the checkerboard](http://127.0.0.1:8766/map-assets/calibration-board.svg)
  in the running app and capture varied views; instructions are in the experiment
  ledger. The rough 40 cm chair-seat reference has not been applied as map scale.

Start capture immediately without pressing a browser button:

```sh
# Pi: begins at the first camera frame, then stops after two minutes.
bash Build/start_mapping_pi.sh --mapping-autostart
```

Use `--mapping-seconds 180` for a three-minute maximum or `--mapping-fps 6` for a
different sample rate. Updated code defaults to 12 clean JPEGs/sec, with up to 24
requested; actual rate depends on camera and disk. The 12 fps update was deployed
and checked on 2 October. IMU synchronization remains a later capture mode. After autostart reaches
its limit, use **Start recording** for another walk without restarting the Pi.

## Installed: recording after Pi boot

The current Pi uses this entry in **evanl1307's crontab**, with no sudo needed:

```cron
@reboot /bin/bash /home/evanl1307/HTN2026/Sharingan/Build/run_mapping_pi_background.sh
```

Cron is active and the same wrapper was successfully launched in the background.
Reboot execution has not yet been tested. The wrapper acquires an exclusive
`flock`, starts mapping-only capture with `--mapping-autostart`, and logs to
`Saved/MappingCapture/service.log`. At each launch it records one two-minute
session, then stays ready for the browser's **Start recording** button.
Boot time counts before you pick it up, so check the displayed recording state.
This is boot startup, not automatic crash recovery. To disable it, remove only
this mapping entry with `crontab -e`; that does not stop an already-running camera.

### Alternative: system-wide service (not installed)

Use only one startup mechanism. Remove the mapping cron entry and stop its camera
before switching to this alternative. Inspect `Build/wallhack-mapping.service`
and adjust its user/paths if they differ. On the Pi:

```sh
sudo install -m 644 Build/wallhack-mapping.service /etc/systemd/system/wallhack-mapping.service
sudo systemctl daemon-reload
sudo systemctl enable --now wallhack-mapping.service
systemctl status wallhack-mapping.service --no-pager
```

Each service start records one bounded walk automatically; the process stays
available for further browser-controlled recordings. Boot time counts before
you pick it up, so check the camera/recording state. Logs:
`journalctl -u wallhack-mapping.service -n 50 --no-pager`.
Disable with `sudo systemctl disable --now wallhack-mapping.service` before
returning to another camera service. This unit is prepared, **not installed**.

## Files and failure behavior

`manifest.json` identifies the recording; `frames.jsonl` links clean images to
frame ID, camera generation, sensor timestamp/exposure when available, and host
monotonic time separately. It does not claim those clocks are synchronized.
Capture uses a four-frame queue, counts drops, reserves 512 MiB of free disk,
and never overwrites a previous session. A stale camera preview is hidden.
An interrupted session is labelled as such; an inconsistent/corrupt archive is
rejected rather than silently called a successful scan.

Laptop reconstruction writes a new `reconstructions/<revision>/` with the COLMAP
database, native sparse models, `points.ply` and `scene.json`. The latest successful
scene is also at the session root. The browser downsamples large point clouds;
the PLY keeps all recovered points. `reconstruction.json` and `reconstruction.log`
report progress/errors. A failed rebuild leaves the previous successful map intact.
Copy important sessions elsewhere: `Saved/` is ignored by Git, not backed up.

If there are no matches/geometry, improve lighting, sharpness, texture and parallax
before buying hardware. A camera that pans without translating cannot establish
room depth. Estimated intrinsics are a baseline; calibrating this exact rotated
image/lens is the next step toward repeatable accuracy. Use the same recordings
to compare ORB-SLAM3/VIO later; a visually plausible cloud is not a measured map.

## Combining Quest later

Both sources can contribute to the same room, but their raw maps do not already
share coordinates or scale. Keep their source maps separate, establish corresponding
static points/planes, solve scale/rotation/translation for the monocular map, then
validate additional landmarks. Quest can supply a floor/wall prior or an independent
reference. It cannot provide an unseen room to a drone without someone scanning
that room first. This import/alignment is not implemented in the baseline.

## Verification on this laptop

- 43 existing Pi tests and 46 existing relay tests pass.
- 33 mapping tests pass: raw image/timing preservation, archive transfer and path
  validation, reconnect/time-limit behavior, error recovery, stale preview, and
  saved-model reload without the Pi. Preview tests cover independent recording
  rate, slow-disk isolation, skipping old frames, unbuffered relay and IPv4 selection.
  Component selection rejects models with many registered cameras but no useful
  3D points, a failure reproduced with the first real recording.
  New checks cover burst metadata/stop/reconnect behavior, capture settings,
  robust depth alignment, stale surface revisions and live-pose expiry, plus
  synthetic lens-calibration recovery. Physical calibration is still pending.
- Actual CPU reconstruction on a **synthetic** 16-image scene: 16 cameras
  registered, 1,887 sparse points, one component. This is software verification,
  not a physical room or drone accuracy result.
- A second test exercised real HTTP archive transfer, import and the reconstruction
  subprocess: 15/16 cameras and 559 points. An exact-16 assertion failed on this
  run, demonstrating that reconstruction quality varies; transfer/build succeeded.
- Browser checked with the Pi offline, with an isolated labelled synthetic model,
  and with the actual live Pi preview and enabled **Start recording** button.
- On the actual Pi, the older checkout's 39 tests pass after deployment. A 60-second
  setup recording saved 165 decodable 640×480 JPEGs, no drops, and per-frame sensor
  timestamps/exposure metadata. Stop and archive transfer through the laptop
  succeeded. Evidence: `Saved/MappingSetup/verification/` on the laptop.
- The standard first-walk baseline recovered 56/236 cameras and 245 points through
  the broader-matching fallback. The optional learned pipeline recovered 224/236
  cameras and 7,865 points. Its second-walk build recovered 97/97 cameras and 9,149
  points through the actual dashboard API. Useful room coverage, metric accuracy
  and actual reboot startup remain unverified. Setup recordings and synthetic
  scenes are separate from these results.
- After the preview improvement, another 51-frame physical recording passed
  download, JPEG decoding and increasing sensor timestamps with zero drops.

```sh
.venv/bin/python -m unittest discover -s Mapping/tests -v
.venv/bin/python -m unittest discover -s SensorRig/CV/tests -v
.venv/bin/python -m unittest discover -s GroundStation/tests -v
```

Backend references: [COLMAP Python API](https://colmap.github.io/pycolmap/index.html),
[COLMAP CPU workflow](https://colmap.github.io/cli.html), and
[Picamera2 completed-request source](https://github.com/raspberrypi/picamera2/blob/main/picamera2/request.py).
