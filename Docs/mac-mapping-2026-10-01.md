# M5 mapping experiment — 1 October 2026

The Mac is sufficient for the **tested DA3 Small workloads**. Local stitching
extends the stool fragment across three windows; neither room sequence maintains
a continuous map under the current checks. This is a CPU/Metal prototype around
our existing runner, not upstream DA3-Streaming or MASt3R-SLAM.

Open `Saved/MappingResearch/mac-m5-20261001/comparison.html`. It shows timings,
memory, staged map growth, rejected updates and a public reference path. No live
Pi, new camera capture, IMU fusion or radar placement was used.

## Measured resources

Local `sysctl`: Apple M5, 25,769,803,776 bytes RAM (24 GiB). PyTorch 2.14.0 reports
Metal available and a 20,401,094,656-byte (19 GiB) recommended GPU working set.
The allocator is capped at 60% of that recommendation (11.4 GiB). RAM is shared
with the OS/apps, not dedicated GPU VRAM.

| Images / processed resolution | First model call | Median of two warm calls | Entire process, three calls | Sampled Metal peak |
| --- | --- | --- | --- | --- |
| 8 / 280×210 | 0.794 s | 0.084 s | 3.67 s | 1.35 GiB |
| 16 / 392×294 | 0.537 s | 0.317 s | 3.07 s | 2.17 GiB |
| 24 / 392×294 | 0.627 s | 0.514 s | 3.82 s | 2.20 GiB |
| 24 / 504×378 | 1.174 s | 0.966 s | 5.74 s | 3.17 GiB |

Inputs: stool recording `20261001T053033Z-f37549ec`, start 48, stride 2. Each
configuration uses a fresh process and the pinned DA3 Small source/checkpoint
in [the runbook](../Mapping/README.md). Model calls exclude preprocessing/export;
process time includes startup, loading, all three calls and NPZ export, but no
stitching/viewer generation. These timings do not establish live latency or FPS.

`Mapping.research_profile` samples process-local Metal allocations every 10 ms.
Driver allocations include caches; sampling may miss brief peaks. RSS and Metal
overlap and must not be added. Raw measurements are in `profiles.json`, each
`profile-*/summary.json`, and `hardware.json`. This does not establish maximum
batch capacity or compatibility with larger models/CUDA-only implementations.
[PyTorch memory-limit API](https://docs.pytorch.org/docs/main/generated/torch.mps.set_per_process_memory_fraction.html).

## Public reference

Downloaded official [TUM Freiburg 1 room data](https://cvg.cit.tum.de/data/datasets/rgbd-dataset/download#freiburg1_room),
CC BY 4.0: 782,381,450 bytes, SHA-256
`5ace47a1d2e53696bc939a84999293a04a7226958e848a609666691fd3cc38da`.
This records the artifact, not comparison with a publisher checksum.
Original metadata hashes and attribution are retained.

`Mapping.reference_benchmark prepare` copies every tenth RGB image without
re-encoding: 137 images spanning 45.339663 s between sampled endpoints. Reference
poses and registered depth are separate, associated by nearest timestamp within
20 ms; unmatched data are withheld. **Inference receives RGB only**, with no
reference poses/depth/intrinsics. Depth PNG values use divisor 5000 from the
[TUM file format](https://cvg.cit.tum.de/data/datasets/rgbd-dataset/file_formats).

Across sixteen 16-image windows advancing eight images at 392×294, camera-position
RMSE after a separately fitted similarity alignment has median **0.0549 m**, range
**0.0247–0.1071 m**. Mean window depth absolute-relative error is **10.24%**, using
that same trajectory-fitted scale. There is no per-image depth scale fitting or
confidence filtering. Valid reference depth is (0,10) m, resized nearest-exact;
missing reference depth is excluded.

Scale, rotation and translation are fitted using evaluated reference positions.
This does **not** establish 5.5 cm independent drone localization, metric-scale
recovery, a reliable full room, or performance on our Pi camera. First-half
alignment with held-out later poses is saved separately in each
`reference-evaluation.json`.

## Overlapping windows

`Mapping.window_replay` aligns shared-image predicted 3D pixels with RANSAC and
a similarity transform. Alternating shared views fit/check the transform. Image
hashes and recording/geometry identity must agree. Shared camera poses supply
additional checks. Existing poses are retained and images contribute surfaces once.

The preset was chosen before these trials: local median pair reprojection ≤2%
image width with sufficient matched pairs; overlap fit inliers ≥60%; held-out
surface median/p90 ≤3%/10% of typical depth; camera-position p90 ≤3% of depth;
orientation p90 ≤10°; sufficient translation; relative scale between 0.2 and 5.
These are **provisional screens**, not calibrated accuracy guarantees.

| Input | Accepted windows | Retained poses | Result |
| --- | --- | --- | --- |
| Stool, starts 48/64/80/96, stride 2 | 3/4 | 32 across 20.72 s | Two extensions; next rejected for camera-position disagreement (3.19% versus 3% threshold) |
| TUM room, starts 0…120 by 8, stride 1 | 1/16 | 16 over 5.00 s | First three windows fail local screening; start 24 initializes; next fails camera-position check |
| Our room, starts 169/185/201, stride 2 | 1/3 | 16 over 10.05 s | Start 185 initializes; next has camera disagreement and insufficient translation |

Initialization can wait for a supported window. Once a retained map loses
alignment, later updates stop; a new origin/scale is never silently spliced in.
Static geometry remains visible; rejected updates add no geometry/current camera
marker. No relocalization, loop closure, global optimization or IMU fusion exists.

All 23 model windows completed: 76.0 s total child-process wall time including
repeated startup/model loading and exports, excluding separate alignment/evaluation.
This is not yet an optimized resident live worker.

## Reproduce and validate

Use `Saved/MappingResearch/da3/venv` for inference and `.venv` for NumPy/OpenCV
analysis. Each inference/replay requires a new output directory. Example:

```sh
Saved/MappingResearch/da3/venv/bin/python -m Mapping.da3_trial \
  --source Saved/MappingResearch/da3/source --model Saved/MappingResearch/da3/model \
  --session Saved/MappingResearch/mac-m5-20261001/tum-room-input \
  --start-frame 0 --stride 1 --frames 16 --resolution 392 --repeats 3 \
  --output Saved/MappingResearch/new-mac-trial

.venv/bin/python -m Mapping.reference_benchmark evaluate \
  --trial Saved/MappingResearch/new-mac-trial \
  --session Saved/MappingResearch/mac-m5-20261001/tum-room-input

.venv/bin/python -m Mapping.window_replay --trials WINDOW_A WINDOW_B WINDOW_C \
  --output Saved/MappingResearch/new-window-replay
```

`Mapping.reference_benchmark` also has bounded safe `extract` and RGB-only
`prepare` subcommands. `Mapping.mac_report` renders this experiment's saved layout.
The final comparison uses `chair-replay`, `tum-replay-v3`, `room-replay-v2`;
earlier diagnostic exports are retained but not displayed. All data/weights and
generated viewers remain under ignored `Saved/`.

**80 mapping tests pass.** New tests cover analytical transforms/scale, outliers,
held-out inconsistent surfaces, reference depth units, timestamp association,
archive traversal/link rejection, and initialization/loss/reference bookkeeping.
Desktop/mobile browser checks pass with no JS errors or page overflow. Visual
review found and corrected clipped camera-path framing.

Next: improve the common pose estimate and calibrate screening on additional
sequences before deployment. A resident model can reduce startup overhead.
Calibrated camera/IMU estimation remains the planned future pose source; a CUDA
machine is optional for comparing algorithms, not required for this Mac prototype.
