# Camera-driven human poses and stronger sensor fusion

Original proposal, 2026-09-20. Implementation and current limitations are recorded in [person-pose-implementation.md](person-pose-implementation.md); the baseline below describes the code before that work.

## Outcome

Render an anatomically proportioned, articulated person whose visible posture follows camera observations. Use radar to improve placement and continuity when vision is missing. Either sensor can independently produce a figure; a camera miss must not become a blanket veto on radar. Keep existing identity colours, bounding boxes, labels, minimap, through-wall rendering and both-eye visibility.

Realism has two separate goals: a better human mesh and better evidence about its pose. A realistic animation is not itself a measurement.

## Current baseline

- `SensorRig/CV/detector/person_detector.py` uses YOLOX person boxes and tracks, without joint landmarks.
- `SensorRig/CV/camera_dashboard.py::camera_position` estimates range from box height and an assumed 1.65 m person. Crouching, varying height and truncation can distort that estimate; clipped boxes already lose range eligibility.
- `SensorRig/CV/radar_service.py::RadarTracker` uses nearest-position association within 450 mm, a 50/50 position blend, three hits to confirm, and 0.4 s track expiry. It does not estimate motion uncertainty.
- `GroundStation/fusion.py` matches temporally nearby camera/radar observations by bearing, with ambiguity rejection and two camera-frame confirmations. Camera and radar retain separate source IDs.
- `WallhackSensorPeopleActor.cpp` renders every live figure at 1.65 m, facing the rig. `WallhackPeopleRenderer.cpp` uses a static mesh. The existing 120 ms interpolation smooths root position, not limbs.
- The installed LD2450 interface supplies up to three target slots containing horizontal coordinates, speed and range resolution. It supplies neither joints nor measured body height, elevation or person-class confidence. Resolution is not a confidence score. [Manufacturer manual](https://h.hlktech.com/download/HLK-LD2450-24G/1/HLK%20LD2450%201T2R%E8%BF%90%E5%8A%A8%E7%9B%AE%E6%A0%87%E6%A3%80%E6%B5%8B%E8%BF%BD%E8%B8%AA%E6%A8%A1%E7%BB%84%E8%AF%B4%E6%98%8E%E4%B9%A6%20V1.02%20.pdf)

The existing Graphify graph covers older HUD code and does not cover the current fusion pipeline; baseline findings above were verified directly in source.

## 1. Establish calibration and a measurable baseline

Record synchronized camera frames, raw radar returns, source timestamps, track outputs and selected manual ground-truth annotations. Keep evaluation recordings separate from tuning recordings. Include empty-room clutter, stationary people, sideways/backwards walking, turns, crouching, two people crossing, partial occlusion, off-camera motion and sensor outages.

Calibrate camera intrinsics and distortion at the actual capture resolution, camera/radar rotation and translation, and camera height/pitch relative to the floor. Start with the measured camera offset from radar: right +32 mm, forward +12.8 mm, up -14 mm. The current floor-plane fusion does not apply that vertical offset; a full projection must. Verify axes and speed sign empirically. Distinguish radar reflection position from the person's foot/pelvis position rather than assuming they coincide exactly.

Keep the rig stationary, as the present registration assumes. Moving the rig requires new registration until rig motion tracking exists. Quest head motion must never be interpreted as person motion.

## 2. Introduce persistent tracks and better radar filtering

Make the laptop fusion tracker the authoritative owner of each fused track ID. Store source-ID links, position, velocity, uncertainty, source ages, pose age and track state. Maintain identity and colour across camera-only, matched and radar-only transitions. Clear unsafe associations on source restart or reference changes.

Replace nearest-position matching with a constant-velocity Kalman filter and a global one-to-one assignment. Compare observations at their capture time, using predicted uncertainty to gate candidates. Use camera bearing, reliable floor position, radar position, motion consistency and association history together. Ambiguous crossings should remain unresolved rather than force an identity swap. Do not average camera/radar estimates equally; weight their measured uncertainty, and avoid counting repeated packets or internally smoothed samples as independent observations.

Apply range/region validation, persistence, implausible-jump rejection and residual checks to radar. Start by evaluating a three-of-five observation confirmation rule against the existing three-hit rule. Do not discard someone solely for standing still. Examine repetitive clutter and reflection patterns across empty-room recordings; tune exclusions only for demonstrated clutter, with every rejection visible on the dashboard. Do not claim that filtering can always distinguish a human from another radar reflector.

Treat missing camera detections according to observability:

| Situation | Behaviour |
| --- | --- |
| Fresh camera person and compatible radar track | Fuse placement; camera supplies person and pose evidence. |
| Camera person, no radar | Keep camera track; use reliable floor projection or explicitly uncertain monocular range. |
| Radar outside camera field of view, occluded, poorly lit or camera unavailable | Keep qualified radar track; camera absence is not contradictory evidence. |
| Radar in a region where a person should clearly be visible, repeated camera misses | Gradually reduce person support and investigate clutter; retain an unverified radar figure while the radar track remains valid. |
| Neither sensor fresh | Expire guidance under the existing freshness limits; any short prediction must be separately labelled and bounded. |

Camera visibility requires evidence. Initially evaluate field of view and image quality; treat occlusion as unknown unless validated depth/occlusion geometry is available in the rig's frame. Quest room geometry would need explicit registration and transport to the laptop before it could inform this decision. Absence from a 2D detector alone does not prove empty space.

Separate track reliability, person evidence, association quality and pose quality. Use categorical states initially; do not present arbitrary scores as calibrated probabilities.

## 3. Add camera pose estimation

Prototype MediaPipe Pose Landmarker on the laptop, alongside the existing detector. It provides 33 body landmarks and visibility values, with configurable multiple-pose support. Benchmark Lite and Full on this laptop and the actual recordings before choosing. Model-relative 3D landmarks have their origin at the hips; they are not absolute room positions or a depth sensor. [Pose model](https://developers.google.com/edge/mediapipe/solutions/vision/pose_landmarker), [coordinate semantics](https://developers.google.com/edge/mediapipe/solutions/vision/pose_landmarker/python).

Send the actual inference frame with its source frame ID and capture timestamp. Do not infer pose from an independently buffered dashboard image and attach it to the newest detector packet. Use a bounded latest-frame queue; a slow pose worker must not block radar, relay or camera capture. Match pose results back to tracks from that frame, accounting for crop/resize transforms. Late poses must not overwrite newer poses or refresh detection age.

Recover visible limb directions, torso orientation and posture. Combine camera-relative pose with fused root position and calibrated floor geometry. Estimate stature and proportions over several reliable observations, then hold them stable; crouching should bend the skeleton rather than shrink the person. Feet-ground intersections can improve camera-only placement when feet are visible and the floor assumption is supported. Otherwise retain an explicit uncertain range.

Use per-joint visibility, temporal consistency and anatomical joint limits. Smooth rotations, handle front/back ambiguity, and fall back only for obscured limbs. Keep body-facing direction separate from direction of travel: sideways and backwards walking must work. Height and body shape remain estimates, with manual override available.

## 4. Render a rigged human and blend inferred movement

Rig and optimize the existing CC0 Blender human base mesh instead of replacing it with unrelated primitives. Add a skeletal mesh, bone weights, mobile LODs and an animation layer. Use joint targets and IK with fixed bone lengths, constrained joints and foot placement. Retarget idle, walking, backward/sideways locomotion and turning animations for fallback. Unreal 5.7 supports retargeting between differently proportioned skeletons and preserving contact points using IK. [Unreal 5.7 retargeting](https://dev.epicgames.com/documentation/en-us/unreal-engine/ik-rig-animation-retargeting-in-unreal-engine?application_version=5.7). Asset provenance: `SourceAssets/HumanSilhouette/BUNDLE_README.txt`; [Blender asset listing](https://www.blender.org/download/demo-files/).

When camera pose is good, observed joints drive the figure. When some joints disappear, blend those limbs toward constrained estimates. When only radar remains, retain the same track's previously estimated stature and briefly its last supported orientation, then blend into an estimated idle/walk animation driven by filtered displacement. Travel direction is only a facing assumption when vision is absent. Radar cannot establish crouching, arm gestures, gaze or actual gait phase; never present those as observed. A radar-only contact never seen by camera receives generic proportions and an explicit unverified/estimated state.

Keep through-wall visibility as requested; this rendering choice does not establish reliable radar detection through a particular wall. Compute bounding boxes from animated body bounds with smoothing, and keep labels anchored consistently. Preserve Hidden mode and validate skeletal materials in both eyes on Quest.

## 5. Extend the protocol and diagnostics

Add a negotiated/versioned track payload with fused ID, sensor provenance, capture/estimate times, root position/velocity/uncertainty, stature source, facing source, pose source/age, landmark validity and per-joint confidence. Maintain a legacy fallback while Pi, laptop and Quest are upgraded separately. Fresh root observations must not make stale joints fresh.

Avoid adding unbounded prediction on top of existing interpolation. Use a common rendering timestamp for root and pose, measure total delay, and test an adaptive interpolation buffer against the current 120 ms behaviour. Keep hard expiry, generation/reset, malformed-packet and tracking-loss protections.

Extend the laptop dashboard with camera skeleton overlays, projected radar points, association residuals, uncertainty ellipses, velocity vectors, joint quality, pose age, rejection reasons and observed/inferred transitions. Add synchronized replay for before/after comparison. Track colours should remain stable; evidence labels should explain source changes without changing identity.

## Delivery and acceptance

Deliver in checkpoints: calibration/recording first; tracker and filtering second; camera skeleton overlay third; animated Quest mesh fourth; end-to-end tuning last. Each checkpoint must remain usable with either sensor disconnected.

Compare held-out recordings against this build for position error, false figures per minute, missed-person duration, identity switches, time to confirm, pose reprojection error and p50/p95 capture-to-display latency. Proposed targets, to confirm after baseline measurement: at least 50% fewer false radar figures on clutter tests without more than a five-percentage-point loss in person recall; p95 live display latency under 300 ms; Quest sustained 72 FPS at the supported eight-figure rendering cap. The radar still has only three simultaneous target slots. These are acceptance goals, not claimed results.

Regression coverage must include duplicate/reordered frames, source restarts, crossings, camera-only/radar-only operation, clipped bodies, no-person frames, occlusion, crouching, backward walking, packet loss, tracking loss, recentering, Hidden, stereo rendering, animated bounds and stale-joint removal. Never satisfy a smoothness metric by retaining expired contacts.

Package with a full Android cook, retain the current APK for rollback, and perform a wearer check with a moving person, a stationary person, an occlusion/reappearance and a crossing. Record camera/radar/dashboard output alongside headset evidence. More detailed models can improve visual realism immediately, but accuracy claims require this measured comparison.
