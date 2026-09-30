# Mapping experiment results — 25 September 2026

This log supersedes the proposed-only status of depth fusion, onboard profiling,
capture controls and known-map localization in ADR-002. Raw recordings and older
reconstruction/inference revisions were preserved. Everything ran locally on the
Mac/Pi; footage was not uploaded to a model service.

## Clarification and physical reference

The false-colour pictures were **relative depth predictions from the normal RGB
camera**, not thermal imagery. Grayscale now replaces the heat-map palette, with
bright = predicted nearer, dark = farther, and explicit no-temperature labels.
Original coloured previews remain as `*-legacy-colormap.jpg` for traceability.

Evan confirmed the second recording mainly covered a chair and estimated its seat
diameter at **roughly 40 cm**. This is stored in the recording's `annotations.json`.
It has not been applied to the map: uncertainty is unknown and the seat endpoints
have not been tied to reliable 3D landmarks. This recording does not establish
whole-room coverage.

## Experiments and decisions

| Experiment | Actual result | Decision |
| --- | --- | --- |
| Relative-depth alignment, chair scan | 74/97 recovered views accepted. Held-out point observations: 7.29% median relative depth error against COLMAP, vs 16.52% for constant depth | Useful as an experimental prior; not physical accuracy |
| Relative-depth alignment, first walk | 55/224 recovered views accepted. Median 9.55% vs 10.45% constant-depth baseline; p90 error 65.3% | Much weaker benefit; reject unsupported regions |
| Chair fusion, permissive checks | 251,251 voxel-averaged inferred points; 8% relative-depth agreement, 2.5 px cycle check, at least two other views | Preserved, but visual inspection shows smearing |
| Chair fusion, stricter checks | **55,726 inferred points**; 3% depth agreement, 1 px cycle check, at least three other views | Current dashboard preset. Fewer filled regions; still visible errors and unverified shape |
| Full geometry rerun, fixed seed | 96/97 views, 8,583 points, 36.6 s mapping | Rerun camera centers differ from the original by 26.9% of path spread after similarity alignment; registration alone is insufficient |
| Overlap-aware offline keyframes | 97 → 78 inputs; 75 recovered, 5,631 points, 26.7 s | Not promoted: loss of coverage and changed shape |
| Reduced refinement | 96/97 views, 8,714 points, 10.3 s | Not promoted: centers differ by 53.7% of original path spread |
| XFeat exported to ONNX | **2,747,186 bytes**; on nine frames all 1,024 keypoints shared with Torch, descriptor cosine ≥0.9999996 | Viable small edge model, benchmarked below |
| Mac ONNX front end, two threads | 12.6 ms median extraction/postprocessing; 24/24 sampled chair pairs with ≥30 verified inliers | Faster implementation candidate; normal reconstruction backend remains the established Torch path |
| Pi ONNX front end, two threads | **85.6 ms** median; 24/24 pairs with ≥30 verified inliers | Suitable candidate for selected-frame processing; not full-rate SLAM |
| Pi ONNX front end, four threads | 90.4 ms median, 124.9 ms p95; voltage/throttling history flags appeared | No speed advantage in this test; prefer two threads for further experiments |
| Pi cheap capture processing | Quality score 0.87 ms; sparse optical flow 5.32 ms, at 320×240 | Quality selection is inexpensive enough to test at capture rate |
| Existing-map localization, saved queries | 20/20 images localized; 24.3 ms median processing, 151 median inliers | Supports a live localization prototype; original SfM used those images, so this is not independent accuracy |
| Existing-map localization, fresh Pi images | 30/30 localized; 92.5 ms median HTTP fetch + processing, 52 median inliers | Dashboard now offers optional camera localization in a saved map |
| Localization negative controls | Black, white, random noise and wrong-resolution inputs returned lost | Loss paths exercised; does not establish a general false-positive rate |

Geometry-only speed trials reuse the same verified feature database. They exclude
feature extraction/matching; the first full run overlapped briefly with a depth
preview experiment. Treat timings as exploratory, not controlled speedup claims.
Numerical solver warnings occurred. No reconstruction variant has ground-truth
camera poses or independently measured room geometry.

Depth alignment fits an affine transform from predicted values to inverse camera-Z
using 80% of eligible tracks. The same point-ID partition is excluded from fitting
in every frame. Held-out errors are relative to **reconstructed** geometry, whose
poses already used those observations; repeated observations are correlated.
They do not imply centimetre accuracy. Fusion rejects invalid/negative depths,
depth edges and weak baselines; inferred provenance remains after cross-view checks.

## Actual capture-profile trials

Four six-second captures were made through the existing camera owner. These are
labelled **CAPTURE TEST** in the dashboard, separate from room/chair scans.

| Profile | Saved frames | Actual median exposure | Median gain | Median luminance / 255 | Dropped frames |
| --- | --- | --- | --- | --- | --- |
| Standard auto | 18 | 41.621 ms | 9.85 | 42.2 | 0 |
| Sharpest burst, auto | 19 | 41.621 ms | 9.85 | 50.3 | 0 |
| Sharpest burst, requested 20 ms | 17 | 19.995 ms | 8.0 | 18.9 | 0 |
| Sharpest burst, requested 10 ms | 17 | 9.992 ms | 8.0 | 29.0 | 0 |

Scene/motion was not controlled, so luminance and sharpness differences are not
a causal blur benchmark. The short-exposure images were dark in the observed
lighting. **Automatic exposure and uniform selection were restored.** The new
controls remain available between recordings and reset to standard after restart.
Capture settings cannot change mid-recording; exact selected-image timestamps and
exposure metadata are retained. Each burst compared about 8–9 candidates. The
longest observed saved-frame gap was 0.666 s, so burst selection still needs a
moving-scene overlap test before becoming the default.

Pi capture files were backed up at
`Saved/MappingSetup/20260925-depth-capture/` on the Pi before deployment. The
camera was restarted under its existing `flock`; the boot cron remains unchanged.
No FC/motor program ran. The read-only inventory found no USB serial flight
controller on this Mac or `/dev/serial/by-id` on the Pi, and no detected Hailo
device/runtime. IMU/Hailo experiments remain blocked on actual hardware inventory.

The two-thread Pi test ended at 47.7°C with no throttling flags. The four-thread
test ended at 48.8°C and `get_throttled=0x50000`: past undervoltage and throttling,
with no active throttling bit at that reading. This is a power-history observation,
not a claim of overheating. See [Raspberry Pi's bit definitions](https://www.raspberrypi.com/documentation/computers/os.html#get_throttled).

## What is available in the dashboard

- **Add AI surfaces / Rebuild AI surfaces:** cached Core ML prediction followed
  by alignment and stricter fusion; writes a separate immutable inference revision.
- **Show:** triangulated points, AI-inferred surfaces, or both. Arbitrary units.
  A layer from an older sparse revision is refused until rebuilt.
- **Start depth preview:** live normal-camera/grayscale pair, capped at 3 fps.
  It does not update the persistent map. Timing labels distinguish inference from
  fetch/inference/render; no glass-to-glass measurement is claimed.
- **Locate camera in this map:** laptop XFeat/ONNX + verified 2D-to-3D matching and
  PnP. No new mapping; the camera must see recognizable features in that saved map.
  Coordinates are hidden on loss, a different map revision, worker exit, or age
  ≥750 ms. The browser also expires the marker if a status request hangs.
- **Capture experiment:** standard, sharpest burst with auto exposure, or burst
  with requested 20/10 ms and gain 8. Check brightness before recording.

Neither live experiment controls flight. Simultaneous inference is an experiment;
the completed 33 mapping tests cover geometric helpers, loss/staleness, API guards,
capture selection and metadata, and synthetic calibration recovery. The existing
43 Pi tests and 46 relay tests pass locally; the Pi's older checkout has 39 passing
tests after this deployment. Reboot execution remains untested.

Final service checks confirmed the camera was ready with automatic exposure and
standard selection, and the current 55,726-point layer was served successfully.
Both optional live workers were left stopped after verification. The browser's
accessibility tree showed the new controls, but the final interactive click-through
could not be completed: desktop automation returned `noWindowsAvailable` while
the page reported itself in the background. API checks are not a substitute for
that remaining UI check.

## Physical camera calibration — capture completed

Evan recorded the displayed checkerboard in `20260925T202722Z-b884439e`.
It stopped automatically after 120 seconds and saved **352 images, zero drops**.
The complete recording is on the Pi and laptop; its dashboard label is
**CAMERA CALIBRATION**. Forty distinct full-board detections were retained.

The first run exposed an OpenCV 5 compatibility issue: the detector returns
`(54, 2)` corners instead of `(54, 1, 2)` on this installation. Normalizing both
formats fixes detection deduplication and calibration, with a regression test.
All three calibration tests pass.

The candidate uses 30 training and 10 held-out views: **0.251 px training RMS,
0.202 px held-out median, 0.511 px p90**. Aggregate corner coverage spans 89.8%
of image width and 77.8% of height. OpenCV intrinsics are approximately
`fx=793.38, fy=793.33, cx=299.11, cy=349.71`; distortion is
`k1=-0.36263, k2=0.18997, p1=-0.000815, p2=0.002367, k3=0`.
Pixel centers follow OpenCV's integer convention; add 0.5 to cx/cy when feeding
COLMAP, matching the existing XFeat keypoint offset.

Twenty random 28-view subset fits yielded 5th–95th-percentile focal lengths
of 785.4–806.0 px. Separately fitting early/late halves gave focal lengths
796.8/782.4 px and held-out median errors 0.157/0.194 px. This supports an
experimental lens candidate, not independent physical map accuracy. The principal
point is visibly off the image center and should not be forced to the center.
The candidate remains unapplied to the dashboard's default reconstruction.

Artifacts: `Saved/MappingResearch/calibration-20260925T202722Z-b884439e/`
contains `candidate.json`, detected corners, `stability.json`, a contact sheet,
an original/corrected lens comparison and isolated calibrated chair trials.

Two chair reconstructions held the candidate intrinsics fixed, reverified the
existing raw feature matches, and used seeds 42/43. Both recovered **97/97 views**
(9,334/9,100 points; mapping 22.3/19.5 s). However their similarity-aligned camera
centers differed by **76.9% of the first trial's path spread**. This is a
repeatability diagnostic, not physical position error. Solver warnings remained.
Lens calibration alone did **not** stabilize the existing chair reconstruction;
neither trial replaced the active dashboard map. A new scan with stronger
translation/overlap and less blur, plus matching/geometry diagnostics, remains
necessary. More checkerboard footage is not the immediate priority.

### Repeating or extending the capture

Lens calibration is now the highest-value next input because repeated sparse
builds changed shape. Open `/map-assets/calibration-board.svg` on a flat screen or
print it without stretching. Record at least 15 genuinely different board views,
keeping all corners visible while changing position, distance and tilt. A stationary
video of one board view is insufficient. Calibration does not require knowing the
physical square size when only estimating intrinsics; it does not set room scale.

```sh
.venv/bin/python -m Mapping.calibrate --session Saved/Mapping/BOARD_SESSION \
  --output Saved/MappingResearch/camera-calibration-candidate.json
```

This fits training views and checks other views, then writes a **candidate**;
it does not silently apply uncertain calibration. Next, use that calibration to
repeat the chair scan, validate dimensions
against a measured reference, and test localization while translating the camera.
Only then promote a continuous mapping backend. ORB-SLAM3/VIO, unseen-surface
completion, Quest alignment and autonomous flight were not implemented in this batch.

## New chair lap with the lens estimate applied

`20260925T203843Z-3fb5185d` saved **222 frames over 74 seconds, zero drops**.
The main clip shows multiple sides of the metal chair; at the end the camera
turns toward the desk and a moving person. Median exposure remained 41.621 ms.
All raw images were retained and copied to the laptop, labelled **CHAIR LAP**.

The reviewed lens candidate was attached explicitly to this recording. The mapper
now validates image geometry and pixel convention, converts OpenCV integer centers
to COLMAP centers, fixes intrinsics during pose estimation and bundle adjustment,
and reverifies learned matches. Each output records the calibration source and hash.
Old maps and future recordings without this opt-in file keep their existing behavior.

The published calibrated XFeat model recovered **222/222 views, 15,453 points,
one component**, with 1.54 px mean internal reprojection error. Feature extraction,
matching, verification and mapping took about 88.4 s after bootstrap preparation.
The strict separate AI layer retained **213,182 points from 160 aligned views**.
Its held-out depth comparison against the sparse reconstruction was 5.19% median,
19.33% p90; these are not independently measured errors or centimetre accuracy.

A separate full-clip build with seed 42 also recovered 222 views (15,990 points),
but its aligned camera-center RMSE was **87.7% of the first model's path spread**.
The existing alignment calculation was cross-checked with PyCOLMAP's independent
Sim3 estimator and matched. Comparing only the first 180 common camera centers
reduced the discrepancy to 30.8% RMSE / 8.5% median of path spread, but still does
not establish reliable shape. The dashboard explicitly warns about unstable shape.

Two additional builds restricted mapping to the first 60 seconds (180 images),
excluding the desk/person ending. Both recovered 180 views, but still differed by
49.2% of path spread after alignment. They were retained as experiments, not
promoted. The ending contributes to instability but is not its sole cause. These
recordings are sufficient for the next software/matching/geometry investigation;
another user capture is not yet requested.

Calibrated-camera localization was exercised on ten saved map images: six passed
the conservative gates, four returned lost. Black, white and random-noise controls
also returned lost. This checks OPENCV-camera integration; these are not independent
query images or an accuracy benchmark. Live workers remain stopped.

All **44 mapping tests** pass, including geometry-mismatch rejection, pixel-center
projection equivalence and removal of stale verified matches after a lens change.
The browser displayed the new model, applied-lens label and inferred-layer counts.
Additional results and source images are under
`Saved/MappingResearch/chair-lap-20260925T203843Z-3fb5185d/`.

## Matcher and solver stability investigation

Twelve new reconstructions tested six settings on the same calibrated 222-image
chair lap. Raw frames, intrinsics and each method's verified matches were held
fixed across its two seeds (42/43). Both matchers used 1,024 XFeat features where
applicable and the same 4,908 temporal/anchor candidate pairs; SIFT used its own
detector. This is a coverage/repeatability study, not a physical accuracy benchmark.

| Setting | Largest-component views, seeds 42 / 43 | Common views | Camera-path disagreement |
| --- | --- | --- | --- |
| SIFT, structure-less fallback off | 96 / 96 | 96 | 0.76% |
| XFeat mutual NN, fallback off | 184 / 183 | 183 | 30.14% |
| XFeat mutual NN, strict geometry | 99 / 100 | 99 | 1.32% |
| XFeat mutual NN, global solver | 222 / 222 | 222 | 92.97% |
| XFeat + LighterGlue, fallback off | 201 / 201 | 197 | 73.88% |
| XFeat + LighterGlue, strict geometry | 105 / 104 | 104 | 14.99% |

Disagreement is camera-center RMSE after official PyCOLMAP similarity alignment,
normalized by the reference path's RMS spread. It uses common views only; the
different rows do not necessarily cover the same parts of the walk. Strict means
no structure-less fallback, at least 60 absolute-pose inliers, 3 px pose residual,
and 2 px point filtering. The ordinary no-fallback trial changes only the fallback
setting. Global positioning and bundle adjustment used CPU, with fixed intrinsics.

The repeatable SIFT and strict-XFeat fragments stop around 37.4–37.7 seconds.
Their 96 common views agree across matchers to 3.88% normalized path RMSE, with
1.51° median orientation difference. The contact sheet shows a blurred swing away
from the chair at 37–39 seconds, followed by a substantially closer view. This is
a plausible tracking break, not proof that blur is the only failure cause. Metal
reflections, repeating shapes, low texture, timing and lens uncertainty remain
possible contributors. The stable fragment is not a complete chair/room map.

LighterGlue used the pinned official XFeat repository's bundled model and Kornia
0.8.3. Extraction took 6.31 s; matching took 120.18 s versus about 2.94 s for the
earlier mutual-NN build. Its ordinary geometry runs took 60.69/69.97 s plus 1.98 s
verification. These are observed local timings with other workloads, not isolated
throughput benchmarks. It did not justify replacing the normal matcher. The global
solver was fast (~6–7 s/run) but produced extreme path outliers and was rejected.

**Implemented:** the dashboard button is now **Experimental matching**. Learned
builds disable structure-less fallback, run two bounded 180-second reconstructions
with seeds 42/43, save the second in `stability-check/`, and attach `quality.json`
to the published scene. A diagnostic warning appears in the viewer even when all
images register. Thresholds are >5% path RMSE, >5° p90 orientation difference, or
<90% shared views. <80% recording coverage is explicitly a partial result. These
are screening heuristics, not measured confidence or accuracy bounds. Standard
SIFT builds retain their prior behavior; existing map revisions are preserved.

**Validation:** 50 mapping tests pass, including similarity-gauge invariance,
distorted-path rejection despite equal view counts, missing-baseline handling,
partial-coverage labelling and warning persistence into both scene files. The
updated verifier also completed two real LighterGlue reconstructions, and a
temporary publisher integration carried the resulting instability warning and
calibration provenance without altering the active map. JavaScript syntax passed.

The local dashboard links to `/map-assets/stability-20260925.html`, with the six
comparisons and frame-coverage bars. Native models, logs, options, reports,
dependency install log, weight hash and breakpoint contact sheet are under
`Saved/MappingResearch/stability-20260925/`. `Mapping.stability_trials`,
`Mapping.matcher_trial` and `Mapping.stability_report` reproduce this workflow.
Upstream references: [XFeat/LighterGlue](https://github.com/verlab/accelerated_features)
and [COLMAP guidance](https://colmap.github.io/faq.html).

**Next:** improve capture continuity through turns with more light, slow sideways
motion, overlapping views and distinct matte detail on the reflective chair. Test
the existing sharper-frame/short-exposure controls against brightness before
another moving capture. No new capture was started in this batch. Once the full
map is repeatable, check physical dimensions; the rough 40 cm seat diameter remains
unapplied. More inferred depth points are not the fix for unstable camera poses.
IMU work still requires an actual FC-to-Pi data connection; Quest remains optional.

## Evidence

All paths below are relative to `Saved/MappingResearch/` (ignored by Git; not a backup):

- `fusion-chair-v1/`, `fusion-chair-strict-v1/`, `fusion-first-v1/`: alignment and fusion diagnostics.
- `chair-fusion-comparison.jpg`, `chair-fusion-strict-comparison.jpg`: camera/sparse/inferred reprojection plots; not independent validation views.
- `chair-speed-trials-v1/`: fixed-seed geometry/keyframe results and native models.
- `models/xfeat-vga.{onnx,json}`, `xfeat-onnx-parity.json`: exported network, provenance and parity.
- `edge-mac-chair.json`, `edge-pi-chair-{2,4}threads.json`: actual hardware benchmarks.
- `capture-profiles-v1/`: camera comparisons and restored settings.
- `chair-localization.json`, `chair-localization-live.json`, `localization-negative-controls.json`: localization trials.
- `live-combined-verification.json`: live depth/localization/capture status samples.
- Session `inferences/` directories retain published depth results and reports.
