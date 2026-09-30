# Mapping research tools

These tools use saved recordings. They do not touch motors or change the Pi's
camera settings. The dashboard's **Experimental matching** option uses XFeat for
correspondences and COLMAP for geometric verification/reconstruction. Its results
are labelled experimental; standard and learned revisions are both retained.
Builds are limited to 12–400 images. See the
[decision and measurements](../Docs/mapping-quality-strategy.md).

The [latest experiment ledger](../Docs/mapping-experiments-2026-09-25.md) records
the subsequent depth fusion, capture controls, actual Pi ONNX benchmarks and live
localization work. Dashboard controls now expose these optional experiments.

## Installed research environment

`Saved/MappingResearch/venv` uses Python 3.12. The main app remains in `.venv`.
Torch and COLMAP must execute in separate processes on this Mac because importing
both in the same process reproduced a duplicate OpenMP runtime crash. Do not
suppress that check with `KMP_DUPLICATE_LIB_OK`.

The upstream XFeat checkout is `Saved/MappingResearch/accelerated_features`,
pinned to `e92685f57f8318b18725c5c8c0bd28c7fe188d9a`. Weights load with
`weights_only=True`. The feature report records their SHA-256.
Apple's ~50 MB F16 Core ML model is under `Saved/MappingResearch/models/`;
`depth-model-source.json` records the repository revision and file checksums.

For a fresh local setup, create a Python 3.12 venv at that path and install
`torch`, `numpy`, `opencv-python-headless` and `tqdm` from PyPI. Clone the official
[XFeat repository](https://github.com/verlab/accelerated_features) to the path
above and check out the pinned commit. Core ML preview additionally requires
`coremltools`, `pillow`, and Apple's
[DepthAnythingV2SmallF16.mlpackage](https://huggingface.co/apple/coreml-depth-anything-v2-small).
Use the versions in `Saved/MappingResearch/requirements-locked.txt` to reproduce
this session. Package/model downloads do not upload recordings.

## Commands

Run a learned reconstruction through the normal publisher and progress file:

```sh
.venv/bin/python -m Mapping.reconstruct --backend xfeat --session Saved/Mapping/SESSION_ID
```

Or use **Experimental matching** in the browser. The publisher retains old revisions
and only replaces the active scene after successful reconstruction.
Learned builds now use fixed seeds 42/43 without structure-less fallback. The
second build is retained in `stability-check/`; `quality.json` reports agreement,
common-view coverage and explicit engineering thresholds. The first result is
still inspectable when unstable, with a warning. Each mapping run has a 180-second
budget, which may produce a partial result. Standard SIFT builds keep their prior
behavior; the automatic two-build check currently applies to learned builds.

Isolated comparisons do not publish or replace any dashboard map:

```sh
.venv/bin/python -m Mapping.stability_trials \
  --session Saved/Mapping/SESSION_ID --source PATH_TO_VERIFIED_DATABASE \
  --output Saved/MappingResearch/NEW_TRIAL --mode strict
```

Modes are `baseline`, `no_fallback`, `strict`, and `global`; seeds default to 42/43.
Use `--sift` instead of `--source` to extract conventional features on the same
temporal/anchor pair schedule. Calibration is loaded from the session opt-in file;
for an existing database it must already match that calibration and have verified
matches. Global CPU mapping is experimental and failed this chair's stability check.

The isolated LighterGlue trial additionally uses `kornia==0.8.3` in the research
venv and the pinned official XFeat repository's bundled `xfeat-lighterglue.pt`.
It records dependency versions, weight checksum and stage timings:

```sh
Saved/MappingResearch/venv/bin/python -m Mapping.matcher_trial \
  --session Saved/Mapping/SESSION_ID --source PATH_TO_DATABASE \
  --repo Saved/MappingResearch/accelerated_features \
  --output Saved/MappingResearch/NEW_MATCHER_TRIAL
.venv/bin/python -m Mapping.verify_feature_db \
  --session Saved/Mapping/SESSION_ID --output Saved/MappingResearch/NEW_MATCHER_TRIAL
```

The first command computes raw correspondences only; the second verifies geometry
and runs the two-build check. LighterGlue is not the dashboard's default matcher.

Compare sparse features and matching on a sample of real frame pairs:

```sh
Saved/MappingResearch/venv/bin/python -m Mapping.benchmark_features \
  --session Saved/Mapping/SESSION_ID \
  --xfeat-repo Saved/MappingResearch/accelerated_features \
  --output Saved/MappingResearch/features.json
```

Try a reversible keyframe subset, with a new output directory:

```sh
.venv/bin/python -m Mapping.keyframe_trial \
  --session Saved/Mapping/SESSION_ID --output Saved/MappingResearch/keyframe-trial
```

Generate labelled, model-inferred relative depth examples:

```sh
Saved/MappingResearch/venv/bin/python -m Mapping.depth_preview \
  --session Saved/Mapping/SESSION_ID \
  --package Saved/MappingResearch/models/DepthAnythingV2SmallF16.mlpackage \
  --output Saved/MappingResearch/depth-preview
```

The depth script writes float arrays, a contact sheet and measured warm inference
timings. Normalization is per image for display: colors do not represent metres
or comparable distances between frames. A separate live preview and fused-layer
worker are now available; they retain inferred provenance and arbitrary scale.

Build the stricter inferred layer through the normal publisher:

```sh
.venv/bin/python -m Mapping.depth_build --session Saved/Mapping/SESSION_ID
```

This caches model predictions by source-image and model hashes, isolates Core ML
in its own process, then fits inverse depth against sparse geometry. Each result
is under `inferences/REVISION/`; `depth.json` is the current layer and records its
source sparse revision. Rebuilding the sparse map invalidates an older layer.

Additional reproducible experiments:

```sh
.venv/bin/python -m Mapping.reconstruction_trials --session Saved/Mapping/SESSION_ID --output Saved/MappingResearch/NEW_TRIAL
.venv/bin/python -m Mapping.edge_benchmark --session Saved/Mapping/SESSION_ID --model Saved/MappingResearch/models/xfeat-vga.onnx --output Saved/MappingResearch/edge.json
.venv/bin/python -m Mapping.localization_trial --session Saved/Mapping/SESSION_ID --network Saved/MappingResearch/models/xfeat-vga.onnx --output Saved/MappingResearch/localization.json
```

For actual Pi profiling, use its `Saved/MappingCapture/SESSION_ID` path and sibling
venv. `edge_features.py`, `edge_benchmark.py` and the 2.75 MB exported ONNX network
are installed on the Pi; Torch is not required there. Export provenance and parity
are beside the model. Current dashboard reconstruction still uses the established
Torch XFeat path; the edge front end is used by the optional laptop localizer.

## Current evidence

- `first-walk-features.json`: SIFT/ORB/XFeat pair benchmark and selection metrics.
- `xfeat-first-walk-isolated/summary.json`: 224/236 views, 7,865 points, ~268 s.
- `keyframes-second-walk/trial.json`: 50 selected images, only 22 recovered;
  simple thinning is not a default capture optimization.
- `depth-second-walk/summary.json`: ~21.3 ms warm inference on Apple M5.
- `depth-second-walk/relative-depth-example.jpg`: one labelled camera/depth pair.
- Second walk's learned reconstruction: 97/97 views and 9,149 points, ~34.4 s,
  stored under its normal `reconstructions/` directory and available in `/map`.

These are empirical results on two walks, not a general performance guarantee.
Learned matching changes correspondence quality; the 3D points are still
geometrically reconstructed. Depth-preview pixels are model predictions. Neither
has established metric accuracy without calibration and independent measurements.
