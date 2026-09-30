wallhack

Start with [current project context](CONTEXT.md) and the
[room-mapping options and next experiments](Docs/room-mapping.md), updated
25 September 2026. [Start the headset-independent mapping baseline](Mapping/README.md):
record a walk on the Pi, reconstruct on the laptop, inspect saved 3D points at
`http://localhost:8766/map`. Installed on the Pi and laptop; physical capture and
download passed. Optional **Stronger matching** recovered 97/97 views in the
second room walk; physical accuracy and metric scale remain unverified. See the
[measured feature/depth experiments and next steps](Docs/mapping-quality-strategy.md).
The [latest experiment results](Docs/mapping-experiments-2026-09-25.md) cover
live depth, localization in a saved map, inferred surfaces and capture controls.

## Sensor rig integration

- [Laptop relay, matching and native Quest silhouettes](GroundStation/README.md)
- [Implementation status and headset acceptance](Docs/quest-ground-station-integration.md)
- [Complete Pi, camera, radar and Quest handoff](SensorRig/HANDOFF.md)
- [Quest browser/native integration contract](SensorRig/Docs/quest_handoff.md)
- [Validation evidence and limitations](SensorRig/Docs/validation/README.md)
