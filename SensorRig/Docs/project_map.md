# Project map and camera handoff

- `CV/`: production camera dashboard, ONNX detector and timestamped tracking,
  optional radar reader and Quest bridge, regression tests and clip evaluator.
  `dashboard.html` + `static/` form the combined `/` and `/quest` views;
  `check_handoff.py` validates live browser/native transport without sending pose.
- `Fusion/`: combined dashboard entry point; PC MSP attitude/dead-reckoning,
  motor controls and incoming/outgoing rig WebSocket in `imu_viz.py`.
- `Radar/`: standalone LD2450 map on 8767 and physical drywall test instructions.
  It shares `CV/radar_service.py`, `radar_web.py` and `radar_trials.py` with the
  combined `/radar` page. Trial recordings are local, ignored JSON artifacts.
- `Firmware/`: independent motor test utility. Camera work does not need it.
- `Source/HandoffQuestHUD/`: Unreal C++ navigation/MRUK scene reconstruction,
  person placement/rendering, spatial test contacts, HUD projection and telemetry.
  `WallhackPeopleSubsystem` is manual placement, not a live detector or identity
  system. `WallhackTelemetrySubsystem` consumes the bridge's world-metre schema.
- `Config/`: Unreal input, Android/passthrough and project packaging settings.
- `Build/`: Windows/Unreal packaging, runtime/render tests and mesh/material
  preparation. These require the Unreal/Quest development environment.
- `Content/` and `SourceAssets/`: fonts, materials and a generic human silhouette
  model, including third-party mesh provenance; not detector training data.
- `Plugins/ModelContextProtocol/`: Unreal editor tooling, not part of sensing.
- `../sensor-test/` (workspace sibling of Sharingan): recorded clips, past model
  comparisons, exploratory model files, and evidence reports. Detector entry
  points forward to `CV/`; historical comparison JSON remains historical.

Original integration gaps: the older camera scripts swapped Picamera2 channels,
reprocessed frames without sequence IDs, and rendered old boxes on newer video.
The fusion README described a WebSocket that the fusion script did not implement.
The Quest HUD also returned to navigation before reaching telemetry rendering.
The shared camera entry point now owns capture exactly once; radar is optional;
the bridge implements the existing schema; `-WallhackBridge` selects telemetry.

The game-plan PDF in this checkout is a Git LFS pointer. Its matching PDF was
retrieved and SHA-256 verified for review (2bb8061a…e579). It describes a direct
Quest browser MJPEG view as the immediate display path, with native spatial
integration separate. It also calls for capture-time alignment and explicit
staleness handling, both implemented in this camera pipeline. Its older Hailo,
AprilTag, rangefinder and flight plans are not treated as currently working
hardware. Binary Unreal assets are project inventory, not executable Python.
