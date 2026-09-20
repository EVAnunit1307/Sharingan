# SensorRig

Start with the [complete sensor/Pi handoff](HANDOFF.md): hardware wiring, boot
configuration, startup/recovery, data flow, validation and remaining Quest work.

Raspberry Pi camera and LD2450 radar, a PC flight-controller IMU, and a Meta Quest
HUD. Start with the [live camera dashboard](CV/README.md), verify people across
body orientations, then enable radar and the WebSocket handoff.

| Folder | Purpose |
|---|---|
| `CV/` | Shared YOLOX person detector, live dashboard, optional radar/Quest services, evaluation and tests |
| `Fusion/` | Combined dashboard launcher and PC IMU/dead-reckoning/MSP utility |
| `Radar/` | Shared LD2450 2D map, standalone launcher and drywall trials |
| `Firmware/` | Independent motor bench tests |
| `Docs/` | Setup, architecture and project map |

Camera-only entry: `CV/pi_camera_stream.py`. Combined entry:
`Fusion/wallhack_dashboard.py`. Both use the same implementation and serve
port 8766; run only one. The optional bridge listens on 8765. Do not run the
standalone radar script alongside combined mode because they share the UART.

The detector uses existing YOLOX nano weights and timestamped confirmation.
Camera position is a rough monocular estimate; radar remains a separate
measurement until calibration. See [setup](Docs/setup.md),
[architecture](Docs/architecture.md), and [project map](Docs/project_map.md).
