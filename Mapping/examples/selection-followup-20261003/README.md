# Saved-footage selection follow-up — paused checkpoint

Open **[the offline progressive replay](index.html)** and select **Replace
streaked frame**. The slider replays the three completed sections. Accepted
sections extend the map; a rejected section preserves earlier geometry and
withholds the current camera marker. This is not a live pipeline.

Evan requested one saved-footage pass followed by a pause. The six planned DA3
jobs, comparisons, presentation checks and saved artifacts are complete. No
further inference, capture or autonomous-flight work is scheduled by this step.

## Result

| Selection | Retained sections, including seed | Selected poses retained | Retained recording span | First / second join camera discrepancy |
| --- | --- | --- | --- | --- |
| Original Large baseline | 2/3 | 32 | 38.86 s | 0.62% / 3.07% |
| Replace the known streaked sample | **3/3** | **40** | **49.02 s** | **1.99% / 2.32%** |
| Local sharpness + feature coverage | 1/3 | 24 | 29.11 s | 7.21% / 0.64% |

Camera discrepancy is the 90th-percentile disagreement between shared estimated
camera centers after surface alignment, divided by median estimated scene depth.
The unchanged threshold is 3%; surface, orientation, translation-baseline and
local consistency checks are unchanged too. The quality case's second pair
passes when tested independently, but its first failure prevents extending the
original map. No silent restart, post-result tuning or threshold relaxation was
used. All three streak-only sections have 23 supported adjacent-image pairs.

Replacing one visibly corrupted sample was useful **in this run**. It improved
the last join enough to retain the full selected span, while the first join's
camera discrepancy increased but stayed below the threshold. This is not proof
of measured drift, dimensions, repeatability, full-room coverage or flight
readiness. The 40 selected poses span part of the 55-second recording; they are
not 658 continuously tracked raw frames. Metric scale and IMU remain absent.

The broader heuristic chose sharper, more spatially distributed features but
made the first join worse. We are **not adopting it as a capture/mapping default**.
Sharpness alone cannot guarantee a stable multi-view estimate.

## Selection and provenance

- Original source: `20261002T213938Z-e3ead809`.
- Forty original anchors: rows 54…639, stride 15. Three 24-view windows begin at
  selected-list positions 0, 8 and 16, sharing 16 views between neighbours.
- The exclusion of rows **363–371** comes from the earlier visual review of
  transient colour streaking. The cause is unknown. This is a manual annotation,
  not a claimed generic corruption detector.
- The streak-only case changes **only row 369 → 372**, repeated consistently in
  every overlapping window. The [source-image strip](streak-selections.jpg)
  shows the original and clean replacement.
- The quality case searches ±6 raw frames around each anchor. Eligible candidates
  receive 70% local Laplacian-sharpness rank and 30% occupied feature-cell rank.
  Nearest time breaks ties. It changes 38/40 selected frames while preserving the
  common sampling bins; the baseline capture and every original image remain intact.
- DA3 Large, reviewed original-K undistortion, 504-pixel processing, two calls
  per process and a 0.6 Metal allocator limit match the earlier experiment. The
  cached baseline is the previously reproduced Large result. Six new model jobs
  cover two alternative selections × three windows. No fine-tuning was performed.

The [plan](plan.json) and all candidate scores/hashes were written before
inference. [Comparison metrics](comparison.json), per-job summaries in `results/`,
the prepared input recordings and screenshots preserve both success and failure.
The new selections were inference-tested once (two calls per process); no
independent rerun or physical ground-truth test is claimed for them. Comparisons
across changed image sets are ablations, not paired pixel-accuracy measurements.

## Reproduce the inference and replay

Use Python 3.12 and [the pinned DA3 dependencies](../../requirements-da3.txt),
as in the [earlier model demo](../room-chain-20261003/README.md). The published
Large checkpoint has a CC BY-NC 4.0 license and remains a research option.
The viewer itself runs offline without Python or weights.

From the repository root, in that Python environment:

```sh
python -m Mapping.saved_footage_trial copy-inputs --output Saved/selection-repeat
python -m Mapping.room_chain_demo fetch --variant large
python -m Mapping.saved_footage_trial run --root Saved/selection-repeat --source Saved/MappingResearch/room-chain-assets/large/source --model Saved/MappingResearch/room-chain-assets/large/model --python python --device mps
```

The first command verifies the bundle and copies only the two prepared input
selections into a new experiment directory. `run` verifies the pinned source,
checkpoint and source-image hashes, then runs the six jobs. Use `--device cpu`
for the explicit slower CPU path; this adapter does not add CUDA support.

Recreate the original baseline from the earlier 40-frame bundle and compare:

```sh
python -m Mapping.room_chain_demo run --variant large --scene couch --device mps --output Saved/selection-baseline
python -m Mapping.saved_footage_trial report --root Saved/selection-repeat --baseline Saved/selection-baseline/large-couch-0 Saved/selection-baseline/large-couch-1 Saved/selection-baseline/large-couch-2
```

Open `Saved/selection-repeat/index.html`. Every inference/replay output directory
must be new. Raw arrays stay under `Saved/`; weights and raw prediction arrays
are not committed. To rerun selection itself, the full original capture is needed:

```sh
python -m Mapping.saved_footage_trial prepare --session Saved/Mapping/20261002T213938Z-e3ead809 --output Saved/selection-new-plan --exclude-range 363 371
```

The prepared selections are bundled so teammates can reproduce inference without
that full archive. Recomputing all candidate scores requires its original images.

Validation: **17 selection/overlap tests passed**. Browser QA checked all three
selections, images, section playback, rejected-section status, mobile layout and
offline loading. Screenshots were inspected. The pause point is this bounded
comparison; future resumption should independently check the retained map against
reference measurements before treating it as physically accurate.
