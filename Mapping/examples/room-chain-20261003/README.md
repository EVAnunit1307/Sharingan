# Recorded-room AI chain and completion experiment

Open **[the offline comparison](index.html)**. It contains four estimates and four
display modes: AI surfaces, floor/wall structure, bounded floor-gap filling, and
an explicitly speculative rectangular envelope. Source images, label overlays,
top-down/orbit controls and a payload budget are bundled. No server, Pi, Internet
or model installation is needed to view the results.

This is a research viewer. All distances are arbitrary, all camera positions are
estimated, and every exported packet has `mapping_eligible: false`, `live: false`
and `unknown_is_free: false`. Nothing is connected to flight control or the live
Quest people protocol. The envelope is a rectangle around visible points, **not
a recovered full room boundary**. Guessed walls may cut through the real room.

## What was tested

Five fresh DA3 jobs ran on the M5: Base and Large on two additional overlapping
couch windows, plus Large on the previously captured wide room sweep. Existing
fixed-input results supply the final couch window and Small room baseline.
All jobs use 24 images at 504-pixel processing resolution, official pinned
weights, two model calls and a 0.6 MPS allocator fraction. They run sequentially.
The comparison reuses SegFormer B0 labels only after exact processed-image hash
verification. No model weights were fine-tuned.

The chain is DA3 depth/cameras → image-matched semantic labels → inferred floor
orientation → vertical wall patches/object regions → separate completion layers.
Depth and semantic confidence remain heuristic scores. A semantic mistake can
turn furniture into a wall patch; fitting that patch does not verify its class.

| Check | Base couch | Large couch |
| --- | --- | --- |
| Ordered windows retained, including seed | 1/3 | 2/3 |
| First join: 90th-percentile shared-camera discrepancy / median depth | 3.12%, rejected | 0.62%, accepted |
| Second join, evaluated independently | 1.39%, passes | 3.07%, rejected |
| Final-window floor samples within tolerance in held-out views | 85.4% | 87.4% |
| Floor additions using all views | 14 | 2 |

The existing 3% camera-discrepancy screen was not changed to accept either near
miss. Base's second pair passing does not repair the first failed join. Large's
two retained windows contain 32 distinct selected views. Neither is a validated
continuous full-room map. The source streak around original row 369 remains in
the input rather than being silently removed.

On the wide sweep, both Small and Large have zero adjacent pairs with sufficient
matches under the existing diagnostic; both stay marked **unstable alignment**.
Large's floor fits held-out samples better (95.8% within tolerance versus 75.9%),
but floor flatness does not validate camera alignment or room dimensions.

The completion evaluation uses even views to create a floor grid and odd views
to check additions. Base proposes 23 training-grid additions: 7 receive floor
support in other views and 5 overlap projected object/wall regions. Large proposes
2: 1 receives other-view floor support and neither overlaps those regions.
Unconfirmed additions are **unverified**, not automatically wrong. Projected
objects may stand above a floor. These checks are not free-space classification.
DA3 saw all images, so holding views out of geometric fitting is not independent
ground truth. Full-view display additions differ from training-grid additions.

See [comparison metrics](comparison.json), [stitch checks](results/stitch-comparison.json),
[execution plan](results/plan.json), and [renders](renders/large-504-local.png).

## Reproduce without the original Pi or Saved directory

The bundle contains 40 couch images and 24 wider-room images, timestamps, reviewed
couch calibration, exact-image cached semantic predictions, pinned model/source
identities, results and screenshots. Checkpoints are downloaded separately.
Large is a **CC BY-NC 4.0** research checkpoint; Small/Base use Apache-2.0
checkpoints. Cached semantics use the same SegFormer model as prior experiments.

Use the pinned Python dependencies in
[Mapping/requirements-da3.txt](../../requirements-da3.txt).
From the repository root, in that environment:

```sh
python -m Mapping.room_chain_demo verify
python -m Mapping.room_chain_demo fetch --variant large
python -m Mapping.room_chain_demo run --variant large --scene couch --device mps --output Saved/room-chain-large-couch
python -m Mapping.room_chain_demo run --variant large --scene room --device mps --output Saved/room-chain-large-room
```

For the Base couch control, fetch/run `--variant base --scene couch`. For the
Small wide-room control, fetch/run `--variant small --scene room`. CPU is an
explicit slower option (`--device cpu`); CUDA has not been added to this adapter.
Existing verified assets can be supplied through `--source` and `--model`.
Each output must be new. Raw predictions, reports, layouts and stitch checks
are preserved. Cached labels are rejected if the processed RGB differs.

To combine reproduced estimates into the four-way viewer:

```sh
python -m Mapping.room_completion --trials Saved/room-chain-base-couch/base-couch-2 Saved/room-chain-large-couch/large-couch-2 Saved/room-chain-small-room/small-room-0 Saved/room-chain-large-room/large-room-0 --labels 'Couch Base' 'Couch Large' 'Room Small' 'Room Large' --output Saved/room-chain-comparison
```

The original UI screenshots can be regenerated with
`Mapping/tests/check_room_completion.cjs`, passing the viewer and a new render
directory. Set `PLAYWRIGHT_MODULE`/`CHROME_BIN` to an installed Playwright/Chrome.

Validation on this Mac: the full existing-plus-new Mapping suite passed **102
tests**; a subsequently added portable-JSON regression and the other six new
geometry tests also pass. Browser QA covered four trials × four approaches,
all bundled images, controls, mobile sizing and offline loading. The three
Large couch windows were rerun from the packaged images in the isolated team
environment: depth, confidence, cameras, intrinsics and processed images are
**array-exact** against the original results, with the same 2/3 retained windows.
See [reproduction evidence](results/reproduction.json). No physical accuracy or
radio/Quest hardware acceptance is implied by these checks.

## Other pipelines: readiness checked, inference not benchmarked

The [source audit](results/upstream-audit.json) pins public revisions. The
[machine probe](results/machine-probe.json) records Darwin/arm64, Metal support,
no CUDA, and the CUDA-capability call's actual failure.

- [DA3-Streaming](https://github.com/ByteDance-Seed/Depth-Anything-3/blob/3d835ec1a5802d64a8b8b15f817a1ab54809bfe4/da3_streaming/da3_streaming.py)
  calls `torch.cuda.get_device_capability()` unconditionally during initialization.
  The default configuration also selects Triton alignment. It needs a port or
  a compatible CUDA environment; our window replay is not DA3-Streaming.
- [VGGT-SLAM 2.0](https://github.com/MIT-SPARK/VGGT-SLAM/blob/35327ac28b7d193df9ccc39ba6346052bb6f1207/vggt_slam/solver.py)
  selects CPU when CUDA is absent, but its prediction path still calls the CUDA
  capability API unconditionally. No complete MPS path was found in the audited
  entry point/solver. It also needs the GTSAM/dependency stack. This is a useful
  next CUDA benchmark, not a failed quality result on our footage.
- [SceneScript](https://github.com/facebookresearch/scenescript/tree/516472d0e62ebbf866f1125cd98c85dec315c100)
  explicitly requires CUDA through TorchSparse and was tested on Linux. Its
  provided models expect Aria semi-dense point clouds. Running it on DA3 output
  additionally requires testing orientation, scale and input-domain mismatch.

We did not download those extra checkpoints, fine-tune them, or claim to run
their inference. Their hardware/input requirements differ from the local chain.

## Deployment beyond the hotspot

See [the deployment and next-step decision](../../../Docs/room-chain-and-deployment.md).
The [payload calculation](results/link-budget.json) uses actual saved JPEG sizes
and serialized layout packets. No physical radio, video codec, packet loss or
autonomous flight was tested. Compact packets were computed on the Mac; sending
them directly from a drone would first require onboard mapping.
