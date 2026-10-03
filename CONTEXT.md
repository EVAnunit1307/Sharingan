# WALLHACK — current context

Updated **3 October 2026**. Read this before older handoffs.

## Offline pose-guided depth and heavier model — 3 October

Evan is disconnected from the Pi and authorized trying pose-guided depth and a
heavier model. Implemented `Mapping.pose_depth` camera preparation, `da3_trial`
Small/Base/undistortion/pose arguments, paired `pose_depth_compare`, and the
reproducible `pose_depth_demo`. No Pi connection, recording, map publication or
production default change. Source/model assets remain in ignored Saved.

Base official checkpoint: revision `f4a6c9b3c95e41c82048423d3493a81ec3fa810e`,
541,518,028 bytes, SHA256 `e01067dc1659613083d9145a9a2547ccdbe6ccbbf83c4fe7b3e8a4e2bdae78b5`.
Same DA3 source and Small checkpoint as prior runs. Fixed couch rows 294…639,
stride 15, 24 images, resolution 504, two sequential model calls, MPS cap 0.6.
Retained coloured-streak row. Raw predicted depth/cameras remain saved separately
from supplied-camera exports; camera agreement is imposed, not validation.

First group undistorted to original K. Paired image errors Small free/guided,
Base free/guided: 3.154 / 3.646 / 1.736 / 3.530 px adjacent; 2.990 / 5.158 /
1.931 / 2.544 px on fixed revisits. All 23 adjacent and three revisit pairs have
enough identical matches. The four cases rebuilt from packaged selected data
with exactly equal output arrays in the isolated team environment. Reproduction
second calls ~1.00 / 0.98 / 2.40 / 2.32 s; first experiment had variable cold/cache
timings. Highest sampled Metal driver allocation ~6.41 GiB. Not live FPS.

Found upstream camera encoder uses only fx/fy, omitting principal point; decoder
assumes W/2,H/2. Reviewed K has a large offset, so ran a separate centered group.
OpenCV alpha=0, exact centered principal point, 1% focal margin; fx/fy ~1,469 vs
~793 originally. All pixels in bounds but substantial crop. Same source selection.
Centered Small free/guided, Base free/guided: 4.620 / 10.280 / 5.899 / 5.686 px
adjacent; 14.568 / 14.401 / 11.748 / 9.363 px revisits. Only 20/23 adjacent pairs
have enough matches, all three revisits supported. Do not compare crop groups
as if pixels/coverage were unchanged. Heavier model is feasible; guidance is
mixed and does not establish a reliable mapped room. Next investigate calibration,
pose consistency and field-of-view-preserving rectification before model escalation.

Local results: `Saved/MappingResearch/pose-depth-20261003/`, viewer.html and
centered-viewer.html, immutable original trials, plans and comparison JSONs.
Team bundle: `Mapping/examples/pose-depth-20261003/` with both viewers, raw selected
images, selected/rebased tracking poses, calibration, labels, source/model pins,
all outcomes and renders. Demo runner defaults to centered inputs;
`--original-principal` reproduces the initial diagnostic. Geometry remains arbitrary
scale, unverified, and ineligible for operator-map or position publication.

## Approximate return confirmed — 2 October

Evan answered the pending mark-A/same-height-and-direction question with
“yes give or take a few”. Treat this as an approximate return only: no distance
or angle units/tolerance were supplied. Exact camera pose remains unmeasured;
the candidate pause windows remain post-hoc. Updated the local review and public
tracking summary with the reply. All numerical results and unvalidated flags are
unchanged. No new recording or inference was started for this clarification.

## Couch AI follow-up — 2 October

Evan said “go”; proceeded offline, with no new capture. This does not confirm the
exact physical return pose. Fixed selection before inference: couch session
`20261002T213938Z-e3ead809`, rows 294…639 at stride 15, 24 views over 28.989 s.
Kept streaked row 369. DA3 Small camera-decoder/MPS: 1.453 s model time, 5.128 s
pipeline; sampled Metal driver peak 3.403 GB. SegFormer supplies optional labels.
The seats/floor are recognizable, with overlapping edges. Opened the standalone
`Saved/MappingResearch/capture-quality-20261002/couch-loop-2137Z/second-pass-draft.html`.

22/23 adjacent pairs have sufficient matches; median pair reprojection 2.283 px
at width 504. AI/ORB camera-position agreement after all-24-view Sim3 fit has
RMSE 18.04% of ORB RMS center spread. First-eight fit / later-sixteen evaluation
is 61.04%. Nearly linear motion (98.75% first PCA axis) weakly constrains alignment
rotation; relative orientation changes instead compare each estimator against its
first camera, median 3.08°, P90 5.58°. Neither estimate is ground truth. No map or
pose packet is published from this draft. Known-pose DA3 conditioning is a next
experiment only; wrapper still uses predicted poses and raw distorted images.

Fixed candidate return rows 318/645: 118 mutual SIFT ratio matches, 98 dominant
homography inliers at a 3 px RANSAC threshold. Median raw displacement 24.59 px;
median XY (-4.75,+22.06) px, homography residual 0.926 px. Image framing changed;
these checks cannot separate physical return error from tracker drift.

Extended the approved public teammate bundle with `--scene couch-return`, 24 new
byte-identical JPEGs, cached labels, render, return comparisons and diagnostics.
The original three estimates are preserved; now 72 selected images and four
estimates. Full 658-frame recording remains in ignored Saved. The saved experiment
plan and `ai-followup.json` retain definitions, original indices and fitted paths.
The later reply confirms an approximate return only. Current physical work remains
a constrained return pose and repeatability;
no new Pi connectivity claim or recording was made.

## Couch return test: 51.4 seconds of continuous tracking — 2 October, 21:39 UTC

Evan positioned the same couch/floor view and requested 15 seconds to set up.
Checked settled 20 ms/gain 4 at uniform 12 fps, then explicitly cued GO after
recording started. A guarded worker stopped after 55 seconds and restored auto
exposure. Recording `20261002T213938Z-e3ead809`: 658/658 images decode, zero
reported queue drops, 55.021 s span, 12.0045 median saved fps, maximum timestamp
gap 208.256 ms. Mean-gray median 40.09, image ORB median 700. Same reviewed lens
candidate applied after checking source/resolution/rotation; lens/focus unchanged
was already confirmed. No additional recording started.

The hotspot transfer was slow/timed out. Evan could not bring the Pi closer.
Preserved partial data and retrieved four resumable byte ranges, then checked
every ZIP CRC before importing. Complete archive is 34,601,200 bytes, SHA256
`c4a7608f0aab7caa9246736c7ef5e385c38c0f44a420da19855727f6301211d3`.
SSH hostname host-key verification succeeded but batch authentication was not
available; do not imply an active SSH control session. HTTP worked during retrieval,
but the final five-second status poll timed out. Last confirmed camera state was
idle with auto exposure restored; do not claim a fresh connectivity check passed.

Default ORB-SLAM3 replay revision `20261002T215421Z-6793fb9a` retains **614/658
poses (93.31%) in one map**, 1,359 landmarks. Rows 0–43 initialize; rows 44–657
are continuously tracked, 3.665–55.021 s (**51.356 s**), with no lost interval.
This meets the single-run 90% continuity target. Three-capture repeatability,
metric scale, physical drift and flight performance remain unvalidated.

Provisional candidate pauses at 25.5–27.5 s and 53–55 s differ by 0.06637
arbitrary units, 15.15% of the estimated excursion to the median pose at 38–41 s;
average orientation differs 3.86 degrees. These windows were selected after
inspection, not ground-truth markers. Actual hand position and tracker error are
mixed. **Do not report this as measured drift or convert to centimetres.** Asked
Evan whether he finished at A with the same camera height/direction; answer is
now received: “yes give or take a few”, an approximate return with no measured
tolerance. The initial and final views differ substantially; the two later pauses
look more alike. Stored images show transient coloured streaking around
30.3–31.1 s; cause unknown, input unfiltered, reported tracking stays active.

Results: `Saved/MappingResearch/capture-quality-20261002/couch-loop-2137Z/review.html`,
`review-summary.json`, contact sheet, return views and artifact neighbours.
Native viewer: `/map-assets/tracking.html?source=tracking&session=20261002T213938Z-e3ead809`.
Browser checks: full images, expected 614/658 and one fragment, no JS errors;
review has no mobile overflow. Native path screenshot visually inspected.
Next: constrain the camera pose on the mark, then test repeatability. The later offline DA3 follow-up is described above.

The earlier team bundle/code/photos/renders were committed and pushed as
`1316b72` after Evan explicitly approved public publication to
`EVAnunit1307/Sharingan`. No further publishing confirmation is needed for that
approved bundle. The unrelated `:memory:.ses` remains untouched.

## Portable team demo requested — 2 October

Evan liked the AI room-draft screenshot and explicitly requested committing the
code, renders and images so teammates can reproduce them. Packaged
`Mapping/examples/room-draft-20261002/`: 48 byte-identical selected JPEGs from the
wide room sweep and brighter sofa/floor recording; one self-contained offline
viewer with standard room, CPU ray-head alternate and brighter sofa estimates;
three PNG renders; cached SegFormer labels; original run summaries; model/source
pins and asset checksums. About 25 MB. No complete recordings, model weights,
virtual environments or dense prediction caches are included. Small demo images
override the repository's LFS image rule so they arrive with an ordinary clone.

`Mapping.room_demo verify` checks the committed data. `fetch` retrieves pinned
official source/model assets under ignored Saved; `run` uses a new output folder,
recomputes DA3 predictions, verifies processed-RGB identity before attaching cached
semantic labels and generates the viewer. Mac Metal/CPU are supported; CUDA and
native Windows are not implemented. The README gives isolated Python 3.12 setup,
commands and limits. `Mapping/tests/check_room_demo.cjs` checks the offline viewer
and re-exports screenshots. Original recording row numbers survive packed input
re-indexing. All three examples then rebuilt in a fresh Python environment and
reproduced all five prediction arrays exactly on this Mac; this is not promised
across hardware/versions. Pinned source/config were freshly downloaded; the model
weights were reused after SHA-256 verification when a redundant download was slow.
Offline/browser/preview checks pass. Per-run evidence and installed environment
are in the demo's `results/`. Full mapping suite: 89 tests pass.

The screenshot concerns DA3 Small's own depth/pose hypothesis, separate from the
successful ORB tracking pass. SegFormer supplies object labels. Alignment, scale,
room-wide continuity and physical accuracy remain unverified. Recommended next:
out-and-back drift/repeatability test, better overlapping views, then evaluate
pose-conditioned DA3 after checking coordinate conventions and lens calibration.
Larger-model comparison and Herman's calibrated/timestamped IMU integration come
later. No new Pi capture or hardware command was issued for this packaging turn.

## Brighter 20 ms pass: first retained Pi tracking path — 2 October, 20:40 UTC

Evan added light and objects, then said ready. Checked the live view and asked
for a downward tilt to include floor. Compared auto / 20 ms / 30 ms at high gain;
with the added light the high-gain manual views clipped the sunlit floor. Then
compared 20 ms/gain 4 against 10 ms/gain 8. Selected 20 ms/gain 4 for better
visible detail (mean gray 58.4 vs 49.8, ~942 vs 888 ORB features in the preview).
Framing/light changed during setup; these are visual profile-selection checks,
not an isolated exposure benchmark. Evidence: `capture-quality-20261002/lit-203617Z/`.

Recorded `20261002T204006Z-fb75b05e` at uniform 12 fps, fixed actual exposure
19,995 us / gain 4. Requested 25-second stop took 27.532 s including HTTP delay.
331/331 images decode; zero reported queue drops; one camera generation;
strictly increasing host/sensor timestamps; median 12.004 fps, maximum gap
124.956 ms. Imported the 21,189,931-byte archive and applied the reviewed,
same-camera lens candidate with provenance. Original images are preserved.
Restored auto exposure after recording; Pi is idle. No further capture started.

ORB-SLAM3 revision `20261002T204333Z-8fdcf0d8` retains **213 consecutive poses**
from row 118 (9.872 s) through row 330 (27.532 s): **17.661 s**, one map, 1,087
landmarks. All preceding frames initialize; no loss after initialization. Coverage
is 64.35% of all input frames, below the 90% development target. Prior darker
pass retained zero poses; image ORB median rose from 208 to 940. Lighting, texture,
motion and exposure changed together. No metric scale or physical accuracy was
validated, and repeatability/return-to-start checks remain pending.

Results: `Saved/MappingResearch/capture-quality-20261002/lit-motion-203958Z/review.html`;
tracking viewer: `/map-assets/tracking.html?source=tracking&session=20261002T204006Z-fb75b05e`.
Browser checks confirmed the initialization/tracking boundary, last pose, one map,
full images and mobile layout; the path/landmark screenshot was visually reviewed.
Saved `tested-profile.json` and added **Well-lit room · 20 ms** (`lit20`) to the
dashboard: uniform sampling, 20,000 us, gain 4, selected fps (tested at 12).
Standard still restores auto. The new preset is available after reloading the
page, and was not applied permanently to the idle Pi.

DA3 dense windows at starts 96/108/120 (24 consecutive views, 504×378) pass
local screens at 1.85/3.42/2.43 px, 23 supported pairs each. Overlap extension
fails camera agreement and translation support, retaining only the first 24
poses. A separate broader AI draft samples rows 120..327, stride 9: 24 images
over 17.244 s, all in the ORB-tracked segment. It has 23 supported image pairs,
2.21 px median reprojection, and local semantic labels. Inspect `room-draft.html`
beside the review. Its learned poses are independent of ORB: fitted full-path
disagreement is 63.6% of ORB path spread, orientation p90 44.5 degrees; an early
8-view fit has worse held-out agreement. These are correlated-estimate comparisons,
not ground truth. **Do not fuse the AI and ORB coordinates as if registered.**

Stage 1 now has a usable baseline for this well-lit scene. Stage 2 is active:
repeat a short, deliberately translated out-and-back loop with known starting
position, then repeatability checks. Maintain the same light, stationary objects,
floor visibility, calibration and 12 fps / 20 ms / gain 4 profile for that test.

## Fuller AI room draft requested and built — 2 October

Evan asked why extra light/texture helps and whether AI can estimate more despite
low confidence. Explained shorter exposure/motion blur and feature tracking, then
built `Mapping.ai_room_draft` plus `Mapping/static/ai-room-draft.html`. New viewer:
`Saved/MappingResearch/ai-room-draft-20261002/room-draft.html` (earlier `draft.html`
uses internal trial names). It shows two room-sweep hypotheses and the recent sofa
fragment, with optional weaker estimates (default on), faint amber uncertainty,
semantic colours, tentative labels, source-frame scrubbing and single-view/orbit
controls. Every hypothesis has its own arbitrary frame; all geometry is inferred,
including higher-ranked points. No new capture was started in this turn.

The exporter reads completed trial arrays/optional semantics and writes only a
new HTML + summary JSON. It does not publish an operator map or pose packet,
change placement thresholds, align different recordings, assign metre units or
complete hidden room boundaries. Tests check world/camera projection, retention
of weak predictions, ineligible-map metadata, unchanged source files, escaping,
overwrite refusal and malformed predictions. These plus existing DA3 tests pass
(8 tests). Browser checks pass on all three estimates: lower-confidence toggle,
semantic colours, source selection, single-view mode, scrub/play controls, no JS
errors and no mobile overflow. Desktop screenshots were visually inspected.

Wider input: `20261002T185247Z-037db03f`, start 192, stride 24, 24 images through
row 744 (~16–62 s into the earlier recording, 46.16 s between endpoints), 504×378.
`room-camera` uses the standard decoder on MPS (1.43 s model call).
Added optional `--ray-pose` to `Mapping.da3_trial`; the pinned official model has
that head. `room-rays` failed on MPS torch.linalg.svd with a Metal pipeline/XPC
error. Explicit CPU retry `room-rays-cpu` succeeded (16.49 s model call), without
editing upstream code. Both broad trials have zero adjacent pairs with >=8
supported matches, so no improvement in mapping accuracy was demonstrated.
The alternate shows a similar warped layout; keeping it visible is intentional
for this hypothesis viewer. The sofa trial retains its previous 1.79 px/23-pair
local diagnostic. Per-scene display: 56,448 sampled points, of which about 22,400
are weak; sampling is for rendering, not extra measured data.

SegFormer labelled both wide inputs (second uses 24 cache hits) and the sofa
trial locally. Candidates include walls/floor/window/door/seating/cabinets/table;
identities can be wrong. All raw arrays, semantic outputs, plans, failures and
manifests are saved. Stage 1 lighting/motion quality remains active; this parallel
draft does not complete continuous localization or the validated room-map stage.

## Short sideways baseline and tracker checks — 2 October, 19:48 UTC

Latest recording: `20261002T194831Z-200bd8f7`. Evan authorized starting the test
and moved sideways while recording. Imported 303 readable images over 25.180 s,
11.994 saved fps, zero reported drops, increasing host/sensor timestamps and a
maximum gap of 83.383 ms. Sofa and floor are visible; motion occurs around 7–14 s.
Automatic exposure remains 41.621 ms. The camera is live and idle, with uniform
selection and 12 fps. Stage 1 is still incomplete: timing passes, but dark
upholstery hides texture and a clear motion capture profile is not established.

Results: `Saved/MappingResearch/capture-quality-20261002/motion-194831Z/review.html`,
`review-summary.json`, `contact-sheet.jpg`, `geometry.html` and native replay logs.
Preview comparison `../194748Z/` tried auto, 30 ms/requested gain 16 and
20 ms/requested gain 16, then restored auto. Actual gains were 9.48 / 10.67 / 10.67;
grayscale means 28.4 / 25.2 / 14.7. Framing moved between samples, so this is an
exploratory visual check, not a controlled quantitative exposure comparison.
Both manual previews lose shadow detail; 10 ms was not tested.

Evan explicitly confirmed camera and focus unchanged since checkerboard capture.
Processed source/resolution/rotation match. Copied the existing passing lens
candidate into this recording with its SHA256, source and application review;
`Mapping.camera_calibration.load` accepts it. This establishes lens applicability,
not metric room accuracy or IMU extrinsics. Original candidate/images are intact.

DA3: three preselected overlapping 24-frame windows at starts 108/120/132,
stride 1, 504×378. All pass local image-consistency checks: 1.79 / 1.33 / 1.58 px,
23 supported pairs each. The first extension fails shared-camera agreement:
p90 position discrepancy 7.25% of typical depth despite 0.62% median held-out
surface discrepancy. Unchanged replay gates retain only the first 24 poses
(1.918 s); no continuous map. The independent path-fit sensitivity comparison
is a different diagnostic and is saved separately. Viewer labels now derive
frame counts/ranges from each trial, with generic recording text and no stale
link to the older chair reconstruction.

ORB-SLAM3 positive control: first 25 seconds of local TUM fr1_room RGB at original
timestamps, official TUM1 intrinsics, BGR adapter setting, no depth/GT/IMU input.
Exported 554/750 poses across three fragments, successfully exercising native
pose/landmark export; this is not a continuous-tracking pass. Pi replay then
retained 0/303 poses, all frames initializing (median 427 extracted features).
Current Pi replay revision `20261002T195226Z-7ede2176`; no estimator thresholds
were relaxed. Next physical action: more scene light and stationary textured
objects at varied depths, then another bounded slow pass. Do not repeat long
walks or describe the local AI fragment as a room map.
The review and three-selection geometry viewer passed Chrome checks for complete
images, correct frame labels, working controls, mobile overflow and JS errors;
both desktop screenshots were visually reviewed. Final Pi status is live/idle.

## Live preview display fixed — 2 October

Evan reported a red strip over a black preview. Pi still JPEGs were complete
640×480 frames. Multipart samples also contained decodable JPEGs, but the native
browser MJPEG display reproduced a blank image with natural dimensions 0×0 while
camera status stayed live. The dashboard revealed the streaming image before
successful decode. Replaced that display path in `Mapping/static/map.js` with
single-flight JPEG requests and offscreen decoding before showing a frame.
It checks JPEG start/end markers, uses frame age plus request/decode time, expires
displayed frames after at most one second, and cancels/guards tab pause/resume.
Preview requests are capped at 10 fps; actual delivery varies. Pi capture remains
12 fps and the HTTP stream endpoint is unchanged. No camera settings/recording
were changed by the fix.

Live browser verification showed complete 640×480 images with advancing frame IDs
and no JS errors; the sofa/backpack is fully visible again. Screenshot:
`/tmp/wallhack-preview-browser-after.png` (temporary). The standalone browser
regression `Mapping/tests/check_preview.cjs` passed partial-download, full-frame,
expiry, hidden-tab, late-response, stale-age, truncated-JPEG and recovery cases.
JavaScript syntax and diff checks passed. Existing open preview pages need a
reload to pick up the updated script. Continue Stage 1 lighting/angle work below.

## Active work: staged mapping/CV handoff — 2 October

Evan authorized working through the mapping/CV milestones in steps, starting now.
He clarified that Herman handles flight and the IMU connection; our scope is
mapping and CV. The active checklist and exit conditions are in
[Docs/mapping-cv-handoff.md](Docs/mapping-cv-handoff.md). Deadline is unspecified.
Stage 1 is camera quality, followed by continuous localization, a repeatable
rough room map, concurrent person detection, and a reproducible software handoff.
The actual raw-IMU versus fused-pose interface and estimator ownership still need
agreement; no messages have been sent to Herman.

The first positioned view showed a sofa/backpack and red wall, with no floor.
A stationary preview comparison of auto
exposure versus 20 ms/gain 8 and 20 ms/requested gain 16 is complete:
`Saved/MappingResearch/capture-quality-20261002/192639Z/`. Actual analogue gains
were 9.48 / 8 / 10.67; grayscale means 57.2 / 28.2 / 35.1. Both 20 ms settings
lose shadow detail. Auto exposure (41.621 ms), uniform selection and 12 fps were
restored. No recording was started during that initial comparison. The later
sideways capture and lens-applicability confirmation are documented above;
lighting remains unresolved. Do not claim static sharpness establishes motion
performance or focus.

## New room walk processed — 2 October

Walk `20261002T185247Z-037db03f` reached the 120-second limit and stopped. Imported
all 1,437 images to the Mac; every image decoded, no reported recorder drops,
increasing sensor/host timestamps, 11.994 saved fps over 119.936 seconds. Maximum
saved-frame gap was 208.455 ms; zero queue drops does not imply perfectly uniform
capture. Median exposure remained 41.621 ms. Footage includes blurred turns,
large blank walls, kitchen/sofa/table views and a stationary doorway ending.

Results: `Saved/MappingResearch/room-walk-20261002/review.html`, linked to
`dense-geometry.html`, `quick-scan.html`, `geometry.html` and the contact sheet.
All inference uses the local DA3 Small MPS runner; no images were uploaded.
The 2/5/10-second excerpts start at row 240 (~20 s), using strides 2/3/6 and
12/20/20 images. All fail image consistency. The 10-second semantic result has
an inferred floor patch from three views, but its 73.69 px reprojection error
and poor correspondence support withhold camera/contact placement. Its top-down
outline is explicitly an unstable diagnostic, not a room map.

Broad replay: 21 windows, 16 images each, stride 6, starts 120..1080 by 48;
**0/21 accepted** with the unchanged gates. Retrospective dense trials at starts
276/336/576 use 24 consecutive images at 504×378. Start 336 (~28 s) passes local
screening (5.65 px, 19 supported pairs) and produces a recognizable sofa/table/
wall fragment over 1.917 s. Tried extensions at 348 and 360: 348 passes its local
screen but disagrees across shared surfaces/poses (camera p90 18.22% of typical
depth, orientation p90 11.23°); the chain retains only the first 24 poses. No
continuous room layout, measured scale or validated localization was established.
Selection plans, raw outputs and JSON diagnostics are preserved; these are
exploratory selections without ground truth. Browser checks passed for all four
pages, duration controls, disabled unsupported placement and mobile overflow;
screenshots were visually reviewed. Next useful capture: a short 10–15 second
sideways pass keeping sofa/table and floor visible, after checking shorter
exposure brightness. The current Pi settings were not changed after the walk.

## Pi recording update deployed and checked — 2 October

Evan is in a larger, better-lit, less-cluttered room, preparing a handheld room
walk. Pi HTTP and the laptop dashboard are live. Evan renewed SSH with
`Build/connect_mapping_pi.sh`; its temporary control socket is available again.
Compared both remote camera modules before deployment: differences were exactly
the prepared FPS update. Installed `camera_dashboard.py` and `mapping_capture.py`,
with remote originals backed up under
`Saved/MappingSetup/fps12-20261002T184842Z-6244c3`. Restarted the idle camera in
mapping-only mode without autostart; boot configuration is unchanged.

Stationary check `20261002T184919Z-f26bfe3c` saved **152 images over 12.586 s**, at
11.997 fps, zero reported drops, zero decode failures, increasing sensor/host
timestamps, maximum saved-frame gap 83.356 ms. The nominal ten-second test took
longer to stop because of HTTP request latency. Results and deployment evidence:
`Saved/MappingSetup/fps12-20261002/`; imported clip: `Saved/Mapping/`.
Pi health before update: throttled `0x0`, 35.1°C, about 30 GB free. Auto exposure
remains 41.621 ms; higher recording FPS does not remove motion blur. Current view
is mostly a blank wall; aim across floor, doorway and furniture for the walk.
Camera is live and **idle**, configured for 12 fps, with a 120-second recording
limit. No room walk was started. Next: Start recording, slow 45–60 second single
room loop, Stop & save, then compare full/short windows on the Mac. IMU remains
unintegrated. Older notes that FPS is local-only are superseded by this check.

## M5 benchmark and window replay completed — 1 October

Evan chose to continue on his M5/24 GB Mac. He also has a GTX 1650 laptop, and
friends may have RTX 50-series GPUs; their exact models/VRAM and availability are
unconfirmed. No messages were sent to them. No Pi access is needed for this work.

Open `Saved/MappingResearch/mac-m5-20261001/comparison.html`. Local M5/24 GiB was
verified; Metal reports a 19 GiB recommended working set. With an 11.4 GiB allocator
cap, DA3 Small warm calls took 0.084 s (8×280), 0.317 s (16×392), 0.514 s (24×392),
0.966 s (24×504). Sampled Metal driver peaks were 1.35/2.17/2.20/3.17 GiB including
caches. Whole processes including three calls/loading/exports took 3.67/3.07/3.82/
5.74 s. These are not live FPS, maximum capacity or upstream CUDA compatibility.

Downloaded official TUM Freiburg 1 room reference data and prepared 137 RGB-only
inputs spanning 45.34 s. Ground-truth poses/depth are separate, used only after
inference. Across 16 short windows, separately similarity-aligned trajectory RMSE
has median 5.49 cm (range 2.47–10.71 cm); mean window depth AbsRel is 10.24% using
the same fitted scale. This does not establish independent metric scale,
continuous room tracking or Pi/drone accuracy. All 23 model windows completed.

New `Mapping.window_replay` aligns shared-image inferred surfaces with RANSAC,
checks held-out shared views and camera agreement, and preserves one map frame.
Stool: 3/4 windows accepted, 32 poses spanning 20.72 s; next window rejected for
camera-position disagreement. Public TUM room and our room each retain one seed;
neither extends reliably. Initialization can wait for a supported window; after
a retained map loses alignment, growth stops. All gates are provisional. No loop
closure, global optimization or IMU fusion is implemented.

All **80 mapping tests passed**; desktop/mobile browser checks passed with no JS
errors or overflow. Browser review caught/fixed reference bookkeeping after late
initialization and clipped camera-path framing. Details and reproduction:
[Mac experiment](Docs/mac-mapping-2026-10-01.md). Final viewer uses `chair-replay`,
`tum-replay-v3`, `room-replay-v2`; earlier exports are retained diagnostics.
Data/weights/generated HTML remain under ignored `Saved/`. Unrelated `:memory:.ses`
is untouched.

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
