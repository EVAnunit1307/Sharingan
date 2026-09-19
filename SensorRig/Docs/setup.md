# Setup

## Raspberry Pi

1. **Network.** The Pi is addressed as `larp-pi.local` (mDNS) over the
   `TS565` WiFi network. If you're testing away from that network, clone a
   hotspot with the same SSID/password from a phone so the Pi joins without
   reconfiguring it — simplest way to avoid re-flashing WiFi credentials in
   the field.
2. **SSH in.** `ssh <your-pi-username>@larp-pi.local`. If `.local` mDNS
   resolution isn't working from your machine, find the Pi's current IP
   from your router and SSH to that instead.
3. **Serial port permissions** (needed for `Radar/ld2450_radar.py` and
   `Fusion/wallhack_dashboard.py`, both of which read `/dev/serial0`):

   ```
   sudo usermod -aG dialout <your-pi-username>
   ```

   Log out of the SSH session and back in — group membership only takes
   effect on a fresh login. After that, the scripts run without `sudo`.

4. **Python dependencies:**

   ```
   pip install flask opencv-python pyserial numpy
   ```

   `picamera2` ships pre-installed on Raspberry Pi OS — if it's missing,
   install it through `apt` (`sudo apt install python3-picamera2`) rather
   than `pip`, since it depends on system camera libraries pip can't
   provide.

5. **(Optional) MobileNet-SSD model files**, for `CV/pi_camera_stream.py`'s
   better detector tier (skip this and it automatically falls back to HOG):
   place `MobileNetSSD_deploy.prototxt` and `MobileNetSSD_deploy.caffemodel`
   under `models/` next to the script.

6. **(Optional) YOLOv8n ONNX model**, for `Fusion/wallhack_dashboard.py`'s
   detector: place `yolov8n.onnx` under `models/` next to the script. Export
   one yourself with `yolo export model=yolov8n.pt format=onnx` (needs the
   `ultralytics` package, only on whatever machine you export from — not
   needed on the Pi itself) or download a pre-exported copy.

7. **Camera orientation.** If the camera is mounted upside down on your
   rig, flip the rotate flag in whichever script you're running — detection
   accuracy noticeably suffers on upside-down input since the detectors are
   trained on upright people, it's not just a cosmetic preview issue.

## PC (ground station)

1. **Find your flight controller's serial port.** `COM4` is the default
   assumed in `Firmware/motor_test.py` and `Fusion/imu_viz.py` — check
   Device Manager (Windows) or `ls /dev/tty.*` (macOS) / `ls /dev/ttyACM*`
   (Linux) and edit the `PORT` constant at the top of whichever script
   you're running if yours differs.

2. **Python dependencies:**

   ```
   pip install flask pyserial websockets
   ```

3. **Point the Pi hostname/IP correctly.** `Fusion/imu_viz.py` assumes
   `larp-pi.local` for both `BRIDGE_URL` (websocket, port 8765) and
   `PI_STREAM_URL` (camera MJPEG, port 8766). Edit both constants if your
   Pi's hostname differs or mDNS isn't resolving on your network — same fix
   either way, point them at the Pi's IP directly.

4. **Props off** before running `Firmware/motor_test.py` or arming test
   mode in `Fusion/imu_viz.py` — see the safety notes in
   [`Firmware/README.md`](../Firmware/README.md).

## Recommended run order

1. On the Pi: either (`CV/pi_camera_stream.py` + `Radar/ld2450_radar.py`)
   **or** `Fusion/wallhack_dashboard.py` alone — see
   [`Docs/architecture.md`](architecture.md) for which mode to pick.
2. On the PC: `Fusion/imu_viz.py`. Confirm the camera pane and radar scope
   both populate before doing anything else — that tells you the Pi
   scripts, the network, and the two port numbers are all correctly lined
   up.
3. Only then, if you need bench motor testing: `Firmware/motor_test.py`
   (with `imu_viz.py`'s motor test-mode left unused, since they share one
   port).
