# Existing flight-controller IMU

## Planned integration with the quick-scan operator sketch — 1 October

Evan confirmed that the IMU **will be available later**. It is a planned input,
not abandoned because the current experiments use only saved camera images.
The quick-scan viewer displays `IMU pending`; every real trial has `imu_used=false`.

`Mapping.pose_bridge.PoseSample` is the boundary between pose estimation and
placing observations in the map. It carries map identity, clock identity,
measurement timestamp, units, position, camera-to-map rotation, tracking state,
pose source and whether IMU data were used. A future calibrated visual-inertial
estimator can provide these poses; the viewer/observation transform need not be
rewritten. The module does **not** estimate pose from raw IMU values, interpolate
pose history or synchronize clocks. It rejects placement on lost/stale poses,
mismatched map/clock/units and invalid rigid transforms.

Integration order after hardware is available:

1. Use the read-only inventory/logger below to identify the FC, actual sensor
   units, available update rate and timestamp limitations.
2. Calibrate camera-to-IMU rotation/translation, gyro/accelerometer bias/noise,
   and the time relationship to camera `SensorTimestamp`. Keep calibration and
   clock-mapping provenance with the recording. USB receive time alone is not
   sensor time.
3. Evaluate a visual-inertial estimator on synchronized saved data. Use inertial
   gravity/orientation to support map orientation and camera motion; establish
   metric scale through a validated estimator/calibration. Raw acceleration
   integration alone is not our room-localization method.
4. Supply camera poses at observation timestamps through the pose boundary and
   register the radar mount/units separately. The LD2450's missing elevation and
   tilted/moving-rig behavior remain separate limitations.

Current semantic floor fitting is only a temporary, inferred reference for the
top-down sketch. It has no measured gravity vector. The contact replay is synthetic
with an identity sensor/camera mount and arbitrary units, so passing that replay
does not validate real IMU/radar fusion. The pose tests cover a future fused pose
at the interface, not a functioning visual-inertial estimator.

On 25 September Evan clarified that the Pi is **not connected to the flight
controller**. The reference to RX was not enough to identify the physical wiring;
his friend handled the hardware. Do not infer an FC data connection from the Pi
being powered or from its camera/radar working.

The old `SensorRig/Fusion/imu_viz.py` expects MSP on Windows `COM4` at 115200
baud. It also sends motor commands, so it is not used for mapping acquisition.
The actual FC model, firmware and IMU chip are still unknown.

Live inventory found no FC USB serial device on either the Mac or Pi. On the Pi,
GPIO14/15 are configured for UART0, `/dev/serial0 -> ttyAMA0`. Earlier physical
tests identified that connection as the LD2450 radar at 256000 baud. Other main
header pins are not configured for another UART. No FC queries were sent and no
wiring, pin configuration or firmware settings were changed.

## First connection

Use the FC's existing USB connector and a **data-capable cable to a Pi USB host
port** for the first bench inventory. Keep propellers off for handling. Do not
move the radar onto the same serial pins. If the FC does not enumerate, identify
its model and power requirements before changing wiring or firmware.

Use the existing Pi environment from its repository root:

```sh
../.venv/bin/python -m Mapping.imu_logger --list
```

The new logger is installed in both the Mac checkout and the Pi's `Mapping/`
directory. Its list-only command runs on the Pi. The Pi already has PySerial for
radar. For another machine install `Mapping/requirements-imu.txt`.
An explicit, confirmed FC device is required; the logger never scans or opens
all ports. Prefer the enumerated `/dev/serial/by-id/...` name on Linux.

```sh
../.venv/bin/python -m Mapping.imu_logger \
  --port /dev/serial/by-id/ACTUAL_FC_DEVICE --baud 115200 \
  --seconds 10 --rate 100 --output Saved/IMU/first-bench-capture
```

Only one process should own that port. Close a configurator or older IMU utility
if it is using the same device. On POSIX the logger requests an exclusive lock.

## What the logger establishes

- Allows only empty-payload MSP queries for API/firmware/board/build identity,
  status and raw IMU. Motor, arming, reboot and configuration commands are absent
  from its whitelist.
- Validates checksums and signed 16-bit IMU payloads. Unknown API major versions,
  unsupported queries or a timeout stop the capture and retain a failure report.
- Saves original accelerometer, gyro and magnetometer protocol values to JSONL.
  Zero magnetometer values do not establish that a magnetometer exists.
- Reports host reply intervals, gaps, round-trip time and distinct observed
  vectors. **100 Hz polling does not establish 100 Hz sensor updates.**
- Retains host request/reply timestamps. MSP_RAW_IMU carries no hardware sample
  timestamp; the request interval does not bound sensor age. Camera SensorTimestamp
  and these host timestamps are not automatically synchronized.

No unit conversion, orientation/position integration or camera fusion is applied.
Scaling depends on actual firmware: even the command name “RAW_IMU” does not
guarantee unprocessed sensor ADC values. Consult the identified firmware's source
before converting. For example, [Betaflight 4.5.2's implementation](https://github.com/betaflight/betaflight/blob/4.5.2/src/main/msp/msp.c)
serializes gyro rate values and acceleration values separately. Its
[protocol definitions](https://github.com/betaflight/betaflight/blob/4.5.2/src/main/msp/msp_protocol.h)
identify the read commands.

After a real capture, check sensor presence, plausible stationary gravity and gyro
values, response to controlled rotations, effective updates and timing. Then
calibrate units, axes, camera-to-IMU mounting and time offset before a VIO trial.
The logger's six unit tests passed; physical FC communication remains untested.
