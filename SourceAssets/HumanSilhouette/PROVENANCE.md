# Human silhouette source

The rendered body is the realistic male human base mesh and its two eye meshes
from **Blender Human Base Meshes v1.4.1**, provided by Blender Studio and community
contributors. These anatomical models serve as both the geometry and proportion
reference. No reconstruction or person recognition is performed.

- Official listing and asset license: https://www.blender.org/download/demo-files/
- Bundle background: https://developer.blender.org/docs/release_notes/3.6/asset_bundles/
- Source download: https://download.blender.org/demo/asset-bundles/human-base-meshes/human-base-meshes-bundle-v1.4.1.zip
- Retrieved: 2026-09-19
- ZIP SHA256: `811F43ACCBB31A88266D932F8F5563B2D13586FCA0BA2693AAD1F5FE582B3515`
- Asset license: **CC0 1.0**, https://creativecommons.org/publicdomain/zero/1.0/

The bundle's embedded README explicitly identifies all provided base-mesh assets
as public domain under CC0. The separate embedded text named `License` describes
a Rain Rig under CC BY 4.0; no Rain Rig objects are used here. The complete
inspection, including both embedded texts, is retained in
`Saved/SilhouetteVerification/source-inspection.json`.

Source objects: `GEO-body_male_realistic`,
`GEO-body_male_realistic.eye.L`, `GEO-body_male_realistic.eye.R`.
The body uses the source's relaxed A-pose. This is a generic adult body shape;
height changes preserve that shape uniformly, rather than measuring an
individual's proportions, clothing, pose or appearance.

Preparation removes sculpt subdivision modifiers and source materials, joins
the eyes, preserves smooth anatomical surfaces, grounds the feet at Z=0, and
normalizes the mesh to one metre tall. Export contains 11,674 vertices and
23,336 triangles. FBX import makes +X the facing axis in Unreal. Runtime scales
uniformly to the specified height, with a default of 1.75 m.

Rebuild with `Build/prepare_human_silhouette.py` in background Blender (disable
auto-execution and enable `--python-exit-code 1`), then
`Build/import_human_silhouette.py` in an Unreal Python commandlet. Downloaded
source files and this attribution are retained for reproducibility; only the
cooked mesh and material are needed on Quest.
