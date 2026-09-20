# Setup and run order

1. Connect to the Pi on the same LAN (`larp-pi.local`; current bench address
   `172.20.10.3`). Use the Pi's actual address if it changes.
2. Use Raspberry Pi OS packages for `python3-picamera2`, OpenCV, NumPy and
   pyserial. Create a virtual environment with access to system packages:

   ```sh
   python3 -m venv --system-site-packages .venv
   .venv/bin/pip install -r Sharingan/SensorRig/CV/requirements.txt
   ```

3. Start camera-only first:

   ```sh
   .venv/bin/python Sharingan/SensorRig/CV/pi_camera_stream.py
   ```

   Open `http://larp-pi.local:8766/`. Check front, side, back, walking, edges,
   empty room and lighting changes. The current mounting uses 180° rotation;
   use `--rotation 0` if remounted upright. Do not run two camera owners.

4. Stop that process, then enable the radar and optional Quest bridge:

   ```sh
   .venv/bin/python Sharingan/SensorRig/CV/pi_camera_stream.py \
     --radar --quest-port 8765
   ```

   The UART is `/dev/serial0` at 256000 baud. The Pi user needs serial access
   (usually membership in `dialout`). Open `/radar` for independent 2D positions.
   The current upright, forward-facing bench mount uses the radar origin with
   X inversion after the physical left/right correction. Follow [the drywall trial procedure](../Radar/README.md)
   before claiming through-wall detection or position accuracy.

5. On the PC, install the `flask`, `pyserial`, and `websockets` dependencies
   and set the COM port and Pi URLs in `Fusion/imu_viz.py`. Its fresh rig pose
   enables world-coordinate camera estimates on the bridge. `--stationary-rig`
   is an explicit fixed-bench alternative, not a moving-rig tracking solution.

6. Open `http://172.20.10.3:8766/quest` in the Quest browser for the combined
   camera/radar view. Run the page's connection check from the headset. See
   [the handoff guide](quest_handoff.md) for the schema and verification script.

7. For the native app, rebuild on the Unreal development machine and launch with
   `-WallhackBridge -WallhackBridgeUrl=ws://<pi-ip>:8765/`. Test the native HUD
   and register the rig to the headset frame before room-anchored use.
   The native client still needs a separate radar-array renderer. This Pi
   workspace cannot build the Unreal Android project.

Motor bench work is independent. Keep propellers removed and follow
[`Firmware/README.md`](../Firmware/README.md) before using motor test mode.
The sensing dashboard does not need motor commands.

See [`CV/README.md`](../CV/README.md) for the measured development results,
CLI options, tests, endpoint contracts, and current limitations.
