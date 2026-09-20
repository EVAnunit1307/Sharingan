"""Exercise the real Pi/relay/native software over loopback with synthetic observations.

Never opens camera/UART hardware or changes the rig's saved calibration. Optional
browser and Unreal checks require their runtimes. Physical accuracy remains a
separate check on the Pi and Quest. All synthetic relay packets are marked replay.
"""
import argparse
import json
import logging
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / "SensorRig/CV")]

import numpy as np
from camera_dashboard import CameraPipeline, create_app as pi_app
from detector.person_detector import PersonDetector
from quest_bridge import QuestBridge
from radar_service import DEFAULT_CONFIG, HEADER, FOOTER, RadarService, RadarTracker
from GroundStation.fusion import FusionConfig
from GroundStation.server import RelayState, create_app, run_upstream, start_websocket
from werkzeug.serving import make_server
from websockets.sync.client import connect


class BoxFixture:
    """Known box enters the production tracker, projection, JPEG and timing path."""
    def __init__(self, detector):
        self.detector, self.cfg, self.last_raw = detector, detector.cfg, []

    def reset(self):
        self.detector.reset()

    def detect(self, frame, timestamp):
        self.last_raw = [dict(box=[280, 40, 360, 440], score=.95)]
        return self.detector.update(self.last_raw, timestamp)


def wait_packet(socket, predicate, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = json.loads(socket.recv(timeout=2))
        if predicate(result):
            return result
    raise AssertionError("Timed out waiting for expected relay output")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "Saved/PiSensorPullTest/Integration")
    parser.add_argument("--browser", type=Path, help="Chromium/Edge executable for dashboard checks")
    parser.add_argument("--unreal", type=Path, help="Built UnrealEditor-Cmd.exe for native socket/actor checks")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    control = out / "fixture-mode.txt"
    control.write_text("live")
    result = dict(physical_hardware_tested=False, input="synthetic camera boxes and LD2450 packets")
    logging.getLogger("werkzeug").setLevel(logging.ERROR)

    detector = PersonDetector()
    blank = np.zeros((480, 640, 3), dtype=np.uint8)
    start = time.perf_counter()
    assert detector.detect(blank, time.monotonic()) == [], "Blank image produced an unexpected detection"
    result["onnx_blank_inference_ms"] = round((time.perf_counter() - start) * 1000, 2)
    detector.reset()
    pipeline = CameraPipeline(BoxFixture(detector), source="synthetic software verification", rotation=0)
    radar = RadarService(config_path=out / "fixture-radar.json")
    radar.set_config(dict(DEFAULT_CONFIG, invert_x=True, mount_level=True, mounting_confirmed=True))
    pipeline.radar = radar
    bridge = QuestBridge(pipeline, host="127.0.0.1", port=0)
    pipeline.bridge = bridge
    stop = threading.Event()
    threads, http_servers = [], []
    downstream = None

    def worker(target, *params):
        thread = threading.Thread(target=target, args=params, daemon=True)
        thread.start()
        threads.append(thread)

    def http(app):
        server = make_server("127.0.0.1", 0, app, threaded=True)
        http_servers.append(server)
        worker(server.serve_forever)
        return f"http://127.0.0.1:{server.server_port}"

    def sensors():
        tracker = RadarTracker()
        raw = HEADER + struct.pack("<HHHH", 0x8000, 3000 | 0x8000, 0x8000, 0) + bytes(16) + FOOTER
        epoch, previous = 1, "live"
        while not stop.is_set():
            try:
                mode = control.read_text(encoding="utf-8-sig").strip()
            except OSError:
                mode = previous
            if mode == "restart" and previous != mode:
                epoch += 1
                pipeline.source_session_id = uuid.uuid4().hex
            previous = mode
            now = time.monotonic()
            if mode not in ("radar_off", "frozen"):
                radar.ingest(raw, tracker, now)
            if mode not in ("camera_off", "frozen"):
                pipeline.publish_frame(blank, now, epoch)
            stop.wait(.1)

    try:
        worker(pipeline.detect_loop)
        worker(sensors)
        bridge.start()
        bridge.port = bridge.server.socket.getsockname()[1]
        pi_url = http(pi_app(pipeline))
        state = RelayState(FusionConfig(alignment_confirmed=True), replay=True)
        downstream, downstream_thread = start_websocket(state, "127.0.0.1", 0, stop)
        threads.append(downstream_thread)
        port = downstream.socket.getsockname()[1]
        relay_url = f"ws://127.0.0.1:{port}/"
        ground_url = http(create_app(state, pi_url, port, out / "fixture-fusion.json"))
        worker(run_upstream, state, f"ws://127.0.0.1:{bridge.port}/", stop)
        with connect(relay_url) as socket:
            def people(packet):
                return packet["spatial_people"]["people"]
            live = wait_packet(socket, lambda p: people(p) and people(p)[0]["position_source"] == "radar_matched")
            identity = people(live)[0]["camera_id"]
            assert people(live)[0]["forward_m"] == 3
            assert live["is_replay"] and not live["rig"]["tracking_ok"]
            (out / "actual-relay-packet.json").write_text(json.dumps(live, indent=2))
            control.write_text("radar_off")
            fallback = wait_packet(socket, lambda p: people(p) and people(p)[0]["position_source"] == "camera_estimate")
            assert people(fallback)[0]["camera_id"] == identity
            control.write_text("camera_off")
            wait_packet(socket, lambda p: not people(p) and p["spatial_people"]["radar_targets"])
            control.write_text("live")
            wait_packet(socket, lambda p: people(p) and people(p)[0]["position_source"] == "radar_matched")
            control.write_text("frozen")
            wait_packet(socket, lambda p: not people(p) and not p["spatial_people"]["radar_targets"])
            control.write_text("live")
            wait_packet(socket, lambda p: people(p) and people(p)[0]["position_source"] == "radar_matched")
        result["pi_bridge_relay_socket_failures_and_recovery"] = "passed"

        if args.browser:
            for name, url in (("pi", pi_url), ("ground", ground_url)):
                command = [sys.executable, str(ROOT / "SensorRig/CV/tests/check_dashboard_browser.py"),
                           "--url", url, "--browser", str(args.browser), "--out", str(out / name)]
                with (out / f"{name}-browser.log").open("w") as log:
                    subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
                result[f"{name}_browser"] = json.loads((out / name / "combined_browser_validation.json").read_text())
            # Also validate the fusion-specific panel rather than just the raw dashboard.
            from playwright.sync_api import sync_playwright
            with sync_playwright() as p:
                browser = p.chromium.launch(executable_path=str(args.browser), headless=True)
                page = browser.new_page(viewport={"width":1440,"height":1100})
                page.goto(ground_url)
                page.locator("#fusion-people .green").wait_for()
                assert page.locator("#fusion-mode").inner_text() == "RECORDED REPLAY"
                control.write_text("radar_off")
                page.locator("#fusion-people .amber").wait_for()
                page.screenshot(path=str(out / "ground/fusion-fallback.png"), full_page=True)
                control.write_text("camera_off")
                page.locator("#fusion-people .blue").wait_for()
                page.wait_for_function("document.querySelectorAll('#fusion-people .green, #fusion-people .amber').length === 0")
                assert "RADAR ONLY" in page.locator("#fusion-people .blue").inner_text()
                page.screenshot(path=str(out / "ground/fusion-radar-only.png"), full_page=True)
                control.write_text("live")
                page.locator("#fusion-people .green").wait_for()
                page.wait_for_function("document.querySelectorAll('#fusion-people .blue').length === 0")
                browser.close()
            result["fusion_browser_source_transitions"] = "passed"

        if args.unreal:
            native = out / "Native"
            native.mkdir(exist_ok=True)
            command = [str(args.unreal), str(ROOT / "HandoffQuestHUD.uproject"), "-unattended", "-nop4",
                       "-RenderOffscreen", "-nocef", "-nosplash", "-nosound", "-nohmd",
                       "-ExecCmds=Automation RunTests SensorSetup.NativeRelay", "-TestExit=Automation Test Queue Empty",
                       f"-WallhackSensorTestUrl={relay_url}", f"-WallhackSensorTestControl={control}",
                       f"-ReportExportPath={native / 'Report'}", f"-abslog={native / 'Tests.log'}"]
            with (native / "stdout.log").open("w") as log:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
            report = json.loads((native / "Report/index.json").read_text(encoding="utf-8-sig"))
            assert report["succeeded"] == 1 and report["failed"] == 0 and report["notRun"] == 0
            result["native_socket_registration_actor_and_failure_tests"] = "passed"
    finally:
        stop.set()
        pipeline.stop()
        for server in http_servers:
            server.shutdown()
        if downstream:
            downstream.shutdown()
        bridge.stop()
        for thread in threads:
            thread.join(timeout=3)
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
