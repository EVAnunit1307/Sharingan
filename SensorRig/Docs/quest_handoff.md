# Camera + radar → Meta Quest handoff

The combined station is running at **http://172.20.10.3:8766/**.
The headset layout is **http://172.20.10.3:8766/quest**.
Both show the actual camera image and LD2450 map together. The Quest layout uses
larger controls, reduces secondary detail and provides whole-page full screen.
It is a 2D browser view, not a native spatial overlay or a WebXR session.

## Open on the headset

1. Connect the Quest to the same reachable Wi-Fi/LAN as the Pi.
2. Open `http://172.20.10.3:8766/quest` in the headset browser. Use the current
   Pi LAN address if it changes; `localhost` on the Quest points to the headset.
3. Check that both camera and radar badges are live. Expand the browser window
   and use **Full screen** if supported. The page falls back to the browser's own
   expand control if the fullscreen API is unavailable.
4. Expand **Meta Quest handoff** and select **Check connection from this device**.
   It opens the real WebSocket and validates a packet. It does not transmit a
   fabricated rig pose. A success on a PC proves PC reachability only; run it on
   the headset too.
5. Walk left/right and confirm the map follows correctly. `invert_x: true` is the
   saved physical correction. Verify loss of camera data leaves radar visible,
   and loss of radar data clears radar positions while the camera continues.

The IP must be reachable across the Wi-Fi network; client isolation can prevent
this even when both devices show the same SSID. If HTTP works but the connection
check fails, test reachability of TCP 8765 as well as TCP 8766.

The combined page retains mounting and drywall recording tools under **Bench
tools**. Their results do not automatically validate through-wall accuracy.
The original `/radar` diagnostic page and standalone radar launcher remain usable.

## Start and verify on the Pi or development computer

Start one station process from the HTN2026 workspace:

```sh
.venv/bin/python Sharingan/SensorRig/CV/pi_camera_stream.py --radar --quest-port 8765
```

Only one process may own the camera/UART. A running process is recorded in
`sensor-test/camera_dashboard.pid`; output is in `sensor-test/camera_dashboard.log`
for the session launched by the coding agent. The command above runs in the
foreground; those PID/log files are not an auto-start service.

The check below reads HTTP assets, an actual JPEG and several real telemetry
packets. It checks fresh, advancing sensor frames and coordinate consistency:

```sh
.venv/bin/python Sharingan/SensorRig/CV/check_handoff.py \
  --url http://172.20.10.3:8766 --seconds 4 \
  --out sensor-test/reports/quest_handoff_validation.json
```

On the Quest development computer, use its Python environment with `websockets`
installed and the same script/URL. No pose or sensor setting is written. Success
means transport and browser handoff are ready; it does not mean a headset was
worn or an Unreal build was tested. The script exits nonzero on failure.

`GET /handoff.json` is the machine-readable connection manifest. URLs use the
host through which the page was reached. It includes mounting, axes, freshness
limits, live sensor/bridge status and explicit unverified integration stages.

## Native integration contract

| Stream | URL / field | Coordinates and use |
|---|---|---|
| Combined browser | `/` or `/quest` on 8766 | Working camera + radar visualization |
| Annotated video | `/stream` on 8766 | MJPEG; boxes are drawn on their inferred image |
| Current image | `/snapshot.jpg` | JPEG; `X-Frame-Id` identifies the result |
| HTTP telemetry | `/detections` | Independent camera/radar freshness and diagnostic metrics |
| Native telemetry | `ws://172.20.10.3:8765/` | JSON packets, `schema_version: 1`, approximately 10 Hz |
| Camera world contacts | WebSocket `detections` | `x`, `y` metres in the supplied rig world frame; `conf` is model score |
| Radar local contacts | WebSocket `drone_relative_radar_targets` | `right_m`, `forward_m` metres in configured drone axes |

The protocol's additional fields are backward compatible with the existing native
camera telemetry parser. A representative **illustrative, non-live** radar entry:

```json
{
  "id": 7,
  "right_m": -0.5,
  "forward_m": 2.0,
  "source": "ld2450",
  "classification": "unverified_radar_target",
  "reference_origin": "radar",
  "confidence": null,
  "timestamp": 1789865000.0
}
```

That point is 0.5 m left and 2 m forward of the radar. This reference has drone
body axes, with the radar as origin for the stationary bench. Offsets to the
drone center can be set later. Positions are already mirrored/rotated/translated
by the server: **do not apply `invert_x` a second time in the Quest client**.
The raw `radar.raw_targets` positions are untransformed mm; legacy `radar.targets`
`x`/`y` are transformed mm. Use the explicit metre fields for new integrations.
Camera IDs and radar IDs are different namespaces and are not person associations.
Radar IDs can reset or swap during reconnects, gaps or crossings.

For an Unreal local frame deliberately aligned to the sensor reference:

- Unreal `X` (forward, cm) = `100 * forward_m`.
- Unreal `Y` (right, cm) = `100 * right_m`.
- Elevation is unknown. The sensor provides no measured Unreal `Z`.

A minimap can use those two axes immediately. A world-anchored overlay must first
register the radar reference origin and orientation to the Quest tracking space.
Do not treat the headset origin, the IMU-integrated rig pose and the radar origin
as interchangeable. Floor/height assumptions must be explicit when rendering 3D.
This stationary bench setup is not validated for a flying or tilted drone.

## Freshness and missing observations

- `camera_connected: false` means camera observations are empty. Camera data
  expires after 750 ms. `camera_frame_id`, `camera_timestamp`, `camera_age_ms`
  identify/age the source image in WebSocket packets.
- `radar.status != "live"` means radar contacts are empty. Packets expire after
  500 ms. `radar.frame_id`, `radar.timestamp` and `radar.age_ms` identify the radar
  frame independently. Mounting must also be confirmed and level for the local
  radar handoff array to contain contacts.
- Rig pose expires after 1000 ms. Without it, world `detections` are empty, while
  fresh local radar contacts can still be delivered. The browser never needs a
  world pose to display the radar.
- The server publishes these limits in `stale_after_ms`. Native clients should
  additionally use a monotonic local receive timer and advancing source frame IDs;
  do not keep drawing the last payload after the connection stalls. Add elapsed
  time since receipt to the source age rather than comparing unsynchronized
  headset and Pi epoch clocks.
- A zero-length radar array does not prove nobody is present. There is no
  per-target radar confidence, elevation, body pose or camera/radar identity match.
  A confirmed software track is not proof of a person behind a wall.

## Native project work still required

The Unreal project already has the `-WallhackBridge` selector and
`-WallhackBridgeUrl=ws://172.20.10.3:8765/` override. Its
`Source/HandoffQuestHUD/Private/WallhackTelemetrySubsystem.cpp` currently parses
`rig` and camera world `detections`; it **does not parse/render the new local radar
array**. A native radar display therefore requires the next implementation step:

1. Add a separate radar-contact model and parse `drone_relative_radar_targets`,
   with explicit reference origin and independent radar age/status. Keep radar
   observations available when camera/world pose is unavailable.
2. Draw those contacts in a sensor-relative minimap first; retain null confidence
   and unknown person classification. Do not populate camera world contacts with
   invented scores or invented rig positions just to make the old HUD draw them.
3. Add radar expiry/connection-loss tests and the known left/right fixture above.
4. Build on the Unreal/Android development machine, deploy to the Quest, then
   verify image readability, controller selection, networking and physical axes.
5. Register the sensor to the headset world only when moving beyond the 2D view.

No Quest headset, adb executable or Unreal/Android build toolchain was available
on this Pi during preparation. There is no newly built/deployed APK or verified
in-headset spatial overlay from this work.

## Verification artifacts

Workspace `sensor-test/reports/` contains `combined_desktop.png`,
`combined_mobile.png`, `quest_dashboard.png`, `combined_browser_validation.json`
and `quest_handoff_validation.json`. Browser failure checks inject controlled
responses only into the test browser; they do not change the real scene or sensor
configuration. Existing camera evaluation and physical radar limitations remain
unchanged. Through-drywall accuracy still requires the recorded bench experiment.
