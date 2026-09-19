# CV — camera + person/object detection

`pi_camera_stream.py` runs on the Pi. It captures from the CSI camera module
via `picamera2`, runs a lightweight object detector on each frame, draws
boxes/labels, and serves the annotated video as an MJPEG stream that
`Fusion/imu_viz.py`'s live pane displays.

## Detector

Detector selection is automatic and layered so the script runs with zero
extra downloads out of the box:

1. **MobileNet-SSD** (20-class VOC model, `models/MobileNetSSD_deploy.*`) —
   used if the model files are present.
2. **OpenCV HOG person detector** (built into OpenCV, no download needed) —
   automatic fallback if the MobileNet-SSD files aren't found.

> Heads up: MobileNet-SSD is a 2016-era 20-class detector and is known to
> confuse people with furniture at odd angles. If you're seeing "chair"
> where a person is standing, that's why — see `Fusion/wallhack_dashboard.py`,
> which upgrades to YOLOv8n for exactly this reason.

## Running it

```
python3 pi_camera_stream.py
```

Then open `http://<pi-hostname-or-ip>:8766/stream` from any device on the
same network, or just point `PI_STREAM_URL` in `Fusion/imu_viz.py` at it and
let the ground-station dashboard embed it automatically.

## Notes

- `DETECT_EVERY_N_FRAMES` lets you trade detection freshness for framerate
  if the Pi is struggling to keep up — bump it to 2 or 3 rather than
  lowering resolution first.
- The camera's default rotation is assumed upright. If yours is mounted
  upside down (common when it's zip-tied to a rig), check the
  `CAMERA_ROTATE_180`-style flag in `Fusion/wallhack_dashboard.py` for the
  equivalent fix — an upside-down feed doesn't just look wrong, it also
  measurably hurts detection accuracy since the detector is trained on
  upright people.
- Only one process can hold the camera at a time — stop this script before
  starting `Fusion/wallhack_dashboard.py` (or vice versa).
