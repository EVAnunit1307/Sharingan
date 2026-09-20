# Firmware — flight controller bench testing

`motor_test.py` is a standalone motor bench-test dashboard. It talks **MSP**
(MultiWii Serial Protocol) to the flight controller over a serial/USB port —
the same wire-level protocol Betaflight/iNav Configurator's "Motors" tab
uses — and sends `MSP_SET_MOTOR` overrides to spin individual motors up for
testing.

It is intentionally separate from `Fusion/imu_viz.py`: that script reads the
same flight controller for live attitude/position, and also has motor
override support built in, but only one program can hold the serial port
open at a time. Reach for `motor_test.py` when all you need is "spin the
motors, check direction and sound," without the rest of the fusion
dashboard running.

## Running it

```
python3 motor_test.py
```

Open the printed local URL in a browser. It serves a small control page and
a Flask JSON API:

| Endpoint | Method | Purpose |
|---|---|---|
| `/status` | GET | Current connection/arm state, per-motor commanded & reported values |
| `/arm` | POST | Enter test-mode (still requires `/unlock` before motors will spin) |
| `/unlock` | POST | Confirms you've read the safety warnings below |
| `/motor` | POST | Command a single motor |
| `/all` | POST | Command all motors at once |
| `/throttle` | POST | Set the shared throttle value used by `/all` |
| `/num_motors` | POST | Tell it how many motors are on this frame (4, 6, 8...) |
| `/estop` | POST | Immediate all-motors-stop |

## Safety — read before running with props on

- **Props off.** This is for bench-testing spin direction and sound, not for
  flight.
- **Disarm the receiver first.** This only works with the FC in its normal
  disarmed state. If your RX is bound and could arm the FC from a stick
  command, unplug it before testing.
- **Built-in watchdog.** If the browser tab stops polling the server for
  more than `HEARTBEAT_TIMEOUT` (0.6s) — tab closed, refreshed, crashed,
  phone locked, backgrounded — all motors are forced to `MOTOR_STOP` (1000)
  and test mode disarms automatically. Don't rely on this instead of taking
  the props off, but it's there.
- **Motor command range is capped** at `MOTOR_SAFE_CAP` (1300) by default,
  well below `MOTOR_ABS_MAX` (2000), specifically so an accidental full-send
  doesn't send a motor to full throttle during a bench check.

## Config

Both `PORT` (`COM4` by default — matches Windows; use something like
`/dev/tty.usbmodemXXXX` on macOS/Linux) and `BAUD` (115200) are constants at
the top of the file — edit them to match your setup rather than passing
flags.
