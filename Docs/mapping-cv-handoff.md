# Mapping and CV handoff to Herman

Updated 3 October 2026. This is the active sequence, with pass conditions to
check before marking a stage complete. No handoff date has been set.

## Goal and ownership

Evan and Codex own camera capture, computer vision, room mapping, the operator
view, replay and the software handoff. Herman owns flight and the IMU hardware
connection. The initial software target is one repeatable room scan and useful
camera-view person detections. Approximate positions on a shared map depend on
validated localization, scale and sensor registration.

Agree whether Herman's interface supplies raw IMU samples or a fused pose.
Raw samples still require visual-inertial estimation and calibration; hardware
availability does not complete that software integration. No message to Herman
has been sent. Flight performance is evaluated with him after ground validation.

## Current baseline

**Latest offline progress:** replacing the known streaked selected frame retains
three DA3 Large couch windows: 40 selected camera estimates spanning 49.02 s.
These pass the unchanged internal screens, not measured physical accuracy.
The [ground-station scene replay](../Mapping/examples/scene-relay-20261003/README.md)
then preserves that draft through delayed/lost updates and a simulated 10-second
interruption, recovering the latest complete map. Camera estimates are withheld
as current because batch inference already takes 7–8 s. The software receiver and
reproducible demo advance Stage 5, without passing room accuracy, people placement,
live latency or IMU integration. Work continues on saved footage; new capture is
the point at which Evan wants to stop.

- **Done:** Pi preview, 12 fps recording, bounded stop, local archive import and
  Mac DA3 inference. Latest room walk: 1,437 readable images, zero reported queue
  drops, 11.994 saved fps. Maximum frame gap was 208 ms.
- **Baseline passed in the brighter scene:** 331 readable frames at 12.004 fps,
  20 ms/gain 4, no reported drops, maximum gap 125 ms. Floor and added objects
  provide clearer image detail. The dashboard now has a Well-lit room preset.
- **At risk:** continuous localization and room mapping. The broad replay
  accepted 0/21 windows. A denser run retained a 1.92-second local fragment but
  failed to extend it. These are not validated full-room results.
- **Initial tracking success:** the brighter Pi pass retains 213/331 poses in one
  map, continuously from 9.9 seconds to the end (17.7 seconds, 1,087 landmarks).
  The first 118 frames initialize. Lens applicability is confirmed, and native
  export also works on reference RGB. The Pi remains mapping-only; concurrent
  person detection still needs validation.
- **Dependency:** IMU connection, timing and calibration are not established.

**Later couch return test, 21:39 UTC:** `20261002T213938Z-e3ead809` retains
614/658 poses (93.3%) in one map, continuously for 51.4 seconds after 3.7 seconds
initializing; 1,359 landmarks. All 658 images decode with zero reported queue
drops. This meets the continuity target for one capture. Candidate later return
pauses differ by 15.1% of the estimated excursion and 3.9° orientation, but the
operator now confirms only an approximate return (“yes give or take a few”),
with no measured tolerance; the pause windows were selected after inspection. This is not measured drift. Repeatability remains pending. Brief
colour streaking at 30.3–31.1 s did not interrupt reported tracking; cause unknown.
Review: `Saved/MappingResearch/capture-quality-20261002/couch-loop-2137Z/review.html`.

The offline AI follow-up uses a preselected second-pass subset, 24 views over
28.99 s (rows 294…639, stride 15), retaining the streaked row 369. DA3 Small runs
in 1.45 s model time / 5.13 s pipeline time on the M5. The local correspondence
check supports 22/23 adjacent pairs at 2.28 px median pair reprojection. The
AI/ORB position disagreement after an all-view similarity fit is 18.0% of ORB's
RMS position spread; fitting only the first eight and evaluating the last sixteen
gives 61.0%. Neither trajectory is ground truth. Seats/floor are recognizable,
with duplicated edges; geometry is still unverified. The portable demo now has a
`couch-return` selection, raw selected inputs, render and the full diagnostics.
The 3 October [conditioning and heavier-model experiment](../Mapping/examples/pose-depth-20261003/README.md)
implements that bridge and compares Small/Base on identical images. Base improves
the first wider-view image check (1.74 vs 3.15 px), but ORB guidance does not.
DA3's camera encoder omits principal-point offset; a second centered-crop test
corrects that input mismatch. Base guidance modestly improves its own check
(5.69 vs 5.90 px adjacent; 9.36 vs 11.75 px revisit), while Small guidance does not.
The strong crop reduces coverage, and the groups are not directly comparable.
The bridge remains experimental. Next: calibration/pose consistency and preserving
field of view; no guided map, physical accuracy or IMU fusion has been validated.

A later same-input Large test after unloading Ollama halves adjacent image
discrepancy against Base (0.86 vs 1.74 px), with 7.4 vs 2.4 s warm model time for
24 images. Large fits the M5 with a sampled 10.12 GiB Metal peak. Its output is
an offline-draft candidate; this does not pass a room-map or localization stage.
Large's checkpoint is CC BY-NC 4.0; the demo records that separately from the
Apache-2.0 Small/Base models. All results and reconstruction inputs are published.

Evidence: `Saved/MappingResearch/room-walk-20261002/review.html` and
[current context](../CONTEXT.md). Original recordings remain intact.

**Additional operator-context draft:** Evan requested showing more tentative AI
geometry even when tracking fails. `Saved/MappingResearch/ai-room-draft-20261002/room-draft.html`
now shows wider-room hypotheses, optional weak surfaces, likely labels and source
images. This can be inspected before Stage 2 succeeds; it does not satisfy the
validated room-map, localization or people-placement exit conditions below.

## Ordered stages

| Stage | Status | Owner | Exit condition | Dependency |
| --- | --- | --- | --- | --- |
| 1. Camera quality and calibration | Baseline passed in the well-lit controlled scene | Evan + Codex | A reviewed capture profile, clear short motion clip, trustworthy timing and an applicable lens calibration | Evan positions/moves the camera |
| 2. Continuous localization | Active: 51.4 s continuous, 93.3% coverage in one run; return validation/repeatability pending | Codex, with Evan's captures | One persistent camera path through an easy loop, reported loss/drift, then repeatable results on three captures | Stage 1; estimator may need replacing |
| 3. Rough operator map | Not started for a validated room | Codex + Evan | Recognizable supported floor/wall/doorway/obstacle arrangement across the repeated captures; unknown space remains unknown | Stage 2 and physical reference checks |
| 4. Person detection alongside capture | Validation pending | Codex + Evan | Recorded person/no-person scenarios reviewed; detection timing, misses, false positives and capture impact measured | Stage 1; map placement also needs Stages 2–3 |
| 5. Reproducible handoff | AI demo/code published and rebuilt in a clean environment; full mapping/CV handoff pending | Codex + Evan; interface agreed with Herman | Clean startup/replay exercise, documented configuration, reference recording/results, logs and a versioned checkpoint | Completed ground checks; IMU interface ownership settled |

### Step 1 — controlled camera baseline

- [x] Confirm the Pi is live and idle at 12 saved fps.
- [x] Identify the exposure issue and review existing calibration provenance.
- [x] Position the camera toward furniture edges and visible floor.
- [x] Save an automatic-exposure baseline and test 20 ms on the same view.
  Inspect actual sensor exposure/gain, brightness, clipping and visible detail.
  Restore the original settings if the trial is unsuitable or interrupted.
- [x] Add light to the sofa/floor, then repeat the 20 ms brightness check while
  holding the framing fixed before the next movement trial. The first view and
  later exploratory comparison were too dark at 20 ms; see the results below.
- [x] Check 10 ms only if 20 ms leaves enough brightness for that comparison.
  Choose from observed image quality; the shorter exposure is not automatically
  better. Static sharpness alone does not measure motion blur.
- [x] Record a bounded clip containing a short sideways pass with the same
  surfaces visible. The 25.2-second baseline includes motion around 7–14 seconds;
  decoding, timestamps, reconnects, drops and gaps pass. Shadows still hide detail.
- [x] Confirm camera/focus unchanged since checkerboard capture and review the
  matching processed source, dimensions and rotation. Apply the candidate to this
  recording with review provenance. Camera/IMU mounting remains a later dependency.
- [x] Establish a usable motion profile after adding light and stationary textured
  objects in view. The subsequent 20 ms/gain 4 pass below establishes a controlled
  baseline; the earlier dark clip alone did not complete Stage 1.

Existing candidate: `20260925T202722Z-b884439e`, 640×480, 180° rotation,
Picamera2; training RMS 0.251 px, held-out median 0.202 px. Those board checks do
not establish present lens applicability, room accuracy or metric map scale.
Matching resolution alone is insufficient to apply it to a new recording.

First stationary comparison, 2 October:
`Saved/MappingResearch/capture-quality-20261002/192639Z/comparison.jpg` and
`report.json`. Auto exposure used 41.621 ms / 9.48× analogue gain; 20 ms with
requested gain 8 applied 8×; requested 16 applied approximately 10.67×. Median
grayscale means were 57.2 / 28.2 / 35.1 and near-black fractions 3.3% / 13.7% /
11.2%. Visual review found lost shadow detail at both shorter-exposure settings.
These preview statistics do not measure motion blur or establish focus. No new
recording was started, and automatic exposure/uniform selection/12 fps were
restored and confirmed. More scene lighting is the next physical action; the
10 ms trial was deferred. The subsequent baseline movement test is below.

Short pass, 2 October, 19:48 UTC: `20261002T194831Z-200bd8f7`, results at
`Saved/MappingResearch/capture-quality-20261002/motion-194831Z/review.html`.
303 readable frames, 11.994 fps, 83.383 ms maximum gap, no reported drops.
The preceding auto/30 ms/20 ms preview comparison (`../194748Z/`) changed framing
between samples, so its brightness figures are exploratory. Auto was restored.
DA3 windows starting at rows 108/120/132 pass local screening at 1.79/1.33/1.58 px,
but shared camera positions disagree and only the first 1.918-second fragment is
retained. Calibrated ORB replay never initializes. A separate TUM RGB control
exports poses successfully but remains fragmented. More lighting and textured
surfaces are the next physical changes; no full-room tracking target has passed.

Brighter follow-up, 2 October, 20:40 UTC: `20261002T204006Z-fb75b05e`, results at
`Saved/MappingResearch/capture-quality-20261002/lit-motion-203958Z/review.html`.
20 ms/gain 4 preserves more detail than the tested 10 ms/gain 8 setting. Actual
exposure stays 19,995 us across all 331 frames. ORB retains 213 consecutive poses
in one map from 9.87–27.53 s, with no loss after initialization. This is 64.4%
overall coverage, below the 90% development target. Physical trajectory accuracy,
metric scale and repeated room coverage remain open. Auto exposure was restored.
Use the new Well-lit room preset at 12 fps for the next same-lighting test.

### Step 2 — establish camera tracking

- Run a calibrated positive-control replay to exercise successful native pose
  export, then replay the improved Pi clip through the existing ORB-SLAM3 adapter.
- Compare with dense DA3 windows using unchanged screening thresholds. Save
  successes and failures, retained poses, frame coverage, gaps and processing time.
- Extend a supported short pass to one room loop. Record where tracking fails,
  whether returning to the start preserves the same map, and start/end discrepancy.
- Repeat on three separate captures. Establish a measured reference before
  reporting errors in metres; otherwise report arbitrary-scale errors explicitly.
- If both methods fail the clearer controlled clip, stop repeating long walks.
  Diagnose calibration/timing versus estimator limits and select the next tested
  tracker or sensor approach. Do not loosen checks solely to display a full map.

Initial working target: at least 90% retained poses on the easy test loop in one
map, with losses shown explicitly. This is a development target, not achieved
performance or a flight-navigation specification. Coverage alone cannot pass it:
layout and return-to-start consistency must also be reviewed.

### Step 3 — turn supported geometry into a rough map

Prioritize visible room boundaries, openings and large obstacles. Retain source
images for each region so Evan can inspect its evidence. Compare a few measured
dimensions and known locations once scale is registered. Report missing areas,
uncertain labels and disconnected fragments. Begin with one room; test a doorway
transition only after the repeatability check passes.

### Step 4 — validate CV with the recorder running

Test no person, a stationary person, a moving person, partial occlusion and leaving
the view. Save observations with image IDs and timestamps. Review misses and
false positives against the footage, measure detector age/throughput, and verify
that old boxes disappear. Confirm inference does not disrupt capture. Retain one
camera owner; choose Pi or laptop inference from measured performance.

Person boxes in images can be delivered before room localization works. Real
map positions require fresh camera pose, compatible scale and calibrated sensor
registration. Simulated contacts remain labelled and separate from real output.

### Step 5 — package the ground-tested version

Provide startup/stop/replay instructions, tested camera settings, hardware/software
versions, calibration provenance, a representative recording and its expected
result, troubleshooting logs, dependency/license notes and a versioned checkpoint.
Exercise restart and connection loss: preserve the saved map, hide stale live
observations and show capture/tracking state. Have a second operator follow the
instructions without relying on this conversation.

Use [the IMU integration notes](../Mapping/IMU.md) and
`Mapping.pose_bridge.PoseSample` for the interface discussion. Required agreements
include timestamp/clock meaning, units, axis conventions, camera/IMU mounting,
map identity, tracking state and ownership of visual-inertial estimation.

## Working rhythm

Work through one physical test at a time. Codex prepares settings and evaluates
the result; Evan positions or moves the camera when prompted. Mark a stage done
only after its evidence is saved. Saved-footage mapping and replay are active; room-wide
mapping remains the main technical risk. Reduce observation time after the
single-room baseline is reproducible.
