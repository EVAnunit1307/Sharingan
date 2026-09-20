import json
from pathlib import Path
import struct
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from radar_service import FrameParser, RadarService, RadarTracker, decode_frame, HEADER, FOOTER
from quest_bridge import QuestBridge, make_packet, valid_pose
from camera_dashboard import CameraPipeline, create_app
from types import SimpleNamespace


def encoded(value):
    return abs(value) | (0x8000 if value >= 0 else 0)


def frame(x=-200, y=1500, speed=-25):
    return HEADER + struct.pack('<HHHH',encoded(x),encoded(y),encoded(speed),0) + bytes(16) + FOOTER


def snapshot():
    return {'camera_connected': True, 'frame_id': 1, 'timestamp': time.time(),
            'people': [{'id': 5, 'score': .9, 'observed': True,
                        'x_m': 1., 'y_m': 2., 'range_source': 'monocular_estimate'}],
            'radar': {'status': 'disabled', 'targets': []}}


class RadarTests(unittest.TestCase):
    def test_signed_coordinates_and_axis_calibration(self):
        self.assertEqual(decode_frame(frame()), [{'x':-200,'y':1500,'spd':-25,'slot':0,'resolution_mm':0}])
        self.assertEqual(decode_frame(frame(),True)[0]['x'],200)

    def test_split_packets_noise_and_bad_footer_recover(self):
        p=FrameParser()
        self.assertEqual(p.feed(b'junk'+frame()[:2]),[])
        self.assertEqual(p.feed(frame()[2:]),[frame()])
        self.assertEqual(p.feed(frame()[:-2]+b'xx'+frame()),[frame()])
        p.feed(b'x'*100000)
        self.assertLessEqual(len(p.buffer),3)

    def test_stationary_zero_velocity_target_is_valid(self):
        self.assertEqual(len(decode_frame(frame(speed=0))),1)
        self.assertEqual(decode_frame(frame(y=7000)),[])

    def test_confirmation_requires_consecutive_matches(self):
        t=RadarTracker();d=[{'x':0,'y':1000,'spd':0}]
        self.assertEqual(t.update(d,1),[])
        self.assertEqual(t.update(d,1.1),[])
        self.assertEqual(t.update([],1.2),[])
        self.assertEqual(t.update(d,1.3),[])
        self.assertEqual(t.update(d,1.4),[])
        self.assertEqual(len(t.update(d,1.5)),1)
        self.assertEqual(t.update([],1.6),[])

    def test_silent_serial_link_clears_targets(self):
        r=RadarService();r.targets=[{'id':1}];r.last_frame=time.monotonic()-1
        self.assertEqual(r.snapshot()['targets'],[])
        self.assertEqual(r.snapshot()['status'],'disconnected')


class BridgeTests(unittest.TestCase):
    def test_stop_closes_an_attached_relay_connection(self):
        from websockets.sync.client import connect
        from websockets.exceptions import ConnectionClosed
        bridge = QuestBridge(SimpleNamespace(snapshot=snapshot), host='127.0.0.1', port=0)
        bridge.start()
        closed = False
        try:
            port = bridge.server.socket.getsockname()[1]
            with connect(f'ws://127.0.0.1:{port}/') as client:
                self.assertEqual(json.loads(client.recv(timeout=2))['schema_version'], 1)
                bridge.stop()
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    try:
                        client.recv(timeout=.5)
                    except ConnectionClosed:
                        closed = True
                        break
                    except TimeoutError:
                        break
                self.assertTrue(closed, 'Stopping the listener must also close existing relay sockets')
        finally:
            bridge.stop()

    def test_connection_manifest_uses_request_host_and_explicit_bridge_port(self):
        pipeline = CameraPipeline(SimpleNamespace(cfg={'model': 'test'}))
        pipeline.bridge = SimpleNamespace(status=lambda: {'status':'listening','clients':0,'port':8765,'pose_status':'awaiting rig pose'})
        client = create_app(pipeline).test_client()
        data = client.get('/handoff.json',base_url='http://192.0.2.17:8766').json
        self.assertEqual(data['quest_url'],'http://192.0.2.17:8766/quest')
        self.assertEqual(data['websocket_url'],'ws://192.0.2.17:8765/')
        self.assertEqual(data['sensors']['radar'],'disabled')
        self.assertEqual(data['native_radar_rendering'],'not_integrated')
        self.assertEqual(data['headset_validation'],'pending')
        self.assertEqual(data['stale_after_ms']['radar'],500)
        data = client.get('/handoff.json',base_url='http://[::1]:8766').json
        self.assertEqual(data['websocket_url'],'ws://[::1]:8765/')
        pipeline.bridge = None
        self.assertIsNone(client.get('/handoff.json').json['websocket_url'])

    def test_combined_and_quest_views_render_tools_and_serve_assets(self):
        pipeline = CameraPipeline(SimpleNamespace(cfg={'model': 'test'}))
        client = create_app(pipeline).test_client()
        for url in ('/', '/quest'):
            response = client.get(url)
            self.assertEqual(response.status_code,200)
            page = response.data.decode()
            self.assertIn('id="feed"',page)
            self.assertIn('id="scope"',page)
            self.assertIn('id="record"',page)
            self.assertNotIn('{%',page)
        self.assertIn('class="quest-view"',client.get('/quest').data.decode())
        for asset in ('dashboard.js', 'dashboard.css'):
            with client.get('/assets/'+asset) as response:
                self.assertEqual(response.status_code,200)

    def test_pose_validation(self):
        self.assertIsNone(valid_pose({'rig':{'tracking_ok':True,'x':float('nan'),'y':0,'heading_deg':0}}))
        self.assertIsNone(valid_pose({'rig':{'tracking_ok':False,'x':0,'y':0,'heading_deg':0}}))
        self.assertIsNone(valid_pose({'rig':{'tracking_ok':True,'x':True,'y':0,'heading_deg':0}}))

    def test_world_transform_matches_clockwise_from_forward(self):
        packet=make_packet(snapshot(),{'x':10,'y':20,'heading_deg':90},'test')
        self.assertAlmostEqual(packet['detections'][0]['x'],12)
        self.assertAlmostEqual(packet['detections'][0]['y'],19)
        json.dumps(packet,allow_nan=False)

    def test_no_contacts_without_pose_camera_or_range(self):
        s=snapshot()
        self.assertEqual(make_packet(s,None,'waiting')['detections'],[])
        s['camera_connected']=False
        self.assertEqual(make_packet(s,{'x':0,'y':0,'heading_deg':0},'test')['detections'],[])
        s=snapshot();s['people'][0]['x_m']=None
        self.assertEqual(make_packet(s,{'x':0,'y':0,'heading_deg':0},'test')['detections'],[])
        s=snapshot();s['people'][0]['observed']=False
        self.assertEqual(make_packet(s,{'x':0,'y':0,'heading_deg':0},'test')['detections'],[])

    def test_invalid_and_expired_pose_clears_previous_fix(self):
        b=QuestBridge(None)
        b.accept_pose({'rig':{'tracking_ok':True,'x':1,'y':2,'heading_deg':0}})
        self.assertIsNotNone(b.pose()[0])
        b.accept_pose({'rig':{'tracking_ok':False}})
        self.assertIsNone(b.pose()[0])
        b.accept_pose({'rig':{'tracking_ok':True,'x':1,'y':2,'heading_deg':0}})
        b.pose_at-=2
        self.assertIsNone(b.pose()[0])


if __name__=='__main__':
    unittest.main()
