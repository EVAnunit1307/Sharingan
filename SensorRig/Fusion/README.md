# Camera, radar, and ground-station integration

## `wallhack_dashboard.py` (Pi)

This is a compatibility launcher for the shared [camera dashboard](../CV/README.md),
with LD2450 radar and the persisted `CV/radar_config.json` mounting settings.
The camera uses YOLOX nano and per-track confirmation. Start with camera-only
validation before enabling radar; do not run multiple camera/serial owners.

```sh
python3 wallhack_dashboard.py
# Optional handoff (requires websockets in the selected Python environment):
python3 wallhack_dashboard.py --quest-port 8765
```

Open `http://larp-pi.local:8766/`. Radar targets remain separate from camera
people until physical alignment and data association are validated. Equal counts
are not evidence of identity. See the camera README for the endpoint schema,
installation, performance checks, and Quest configuration.

## `imu_viz.py` (runs on the PC — the ground station)

The main ground-station dashboard. Reads the flight controller's live
attitude (roll/pitch/yaw) and raw IMU over **MSP** on a serial/USB port
(`COM4` by default), and derives a rough position estimate via
gravity-compensated acceleration integration with gyro-gated zero-velocity
updates (ZUPT) — not a Kalman filter, just enough dead-reckoning to place a
marker on a minimap, and it drifts like dead-reckoning always does.

It then bridges to the Pi over a **WebSocket** (`BRIDGE_URL`,
`ws://larp-pi.local:8765/`): the drone/rig's IMU pose is sent to the Pi as
the real reference frame for its person-detection fusion (replacing an
earlier `fake_rig.py` stub), and the Pi's detections come back to be
plotted here relative to the rig. It also pulls in the Pi's raw camera feed
directly (`PI_STREAM_URL`, `http://larp-pi.local:8766/stream`) for a live
picture-in-picture pane.

Motor test-mode (arm/unlock/per-motor or all-motor throttle, the software
watchdog) is merged into this same process using the identical mechanism as
`Firmware/motor_test.py`, so you don't need a second program fighting over
the COM port if you want to spin motors while watching live attitude.

```
python3 imu_viz.py
```

| Endpoint | Purpose |
|---|---|
| `/attitude` | Live roll/pitch/yaw/position JSON |
| `/reset` | Zero the position/velocity estimate (do this on a level, stationary bench) |
| `/arm`, `/unlock`, `/motor`, `/all`, `/throttle`, `/num_motors`, `/estop` | Same motor test-mode API as `Firmware/motor_test.py` |

**Only one of `imu_viz.py` or `motor_test.py` can hold the COM port at a
time.**
