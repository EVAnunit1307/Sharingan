import json
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from camera_dashboard import CameraPipeline, create_app
from quest_bridge import make_packet
from radar_protocol import COMMAND_HEADER, COMMAND_FOOTER, ReplyParser, read_diagnostics
from radar_service import DEFAULT_CONFIG, RadarService, RadarTracker, decode_frame, sensor_to_drone, validate_config
from radar_trials import RadarTrials, summarize
from test_handoff import frame


def reply(command, data=b'', status=0):
    body = struct.pack('<HH', command | 0x100, status) + data
    return COMMAND_HEADER + struct.pack('<H', len(body)) + body + COMMAND_FOOTER


class DiagnosticUART:
    def __init__(self, reject=None):
        self.buffer = b''
        self.commands = []
        self.reject = reject

    @property
    def in_waiting(self):
        return len(self.buffer)

    def write(self, packet):
        command = struct.unpack_from('<H', packet, 6)[0]
        self.commands.append(command)
        data = {0xff: b'\x01\x00\x40\x00', 0xa0: struct.pack('<HHI',1,0x0204,0x23101915),
                0x91: struct.pack('<H',2), 0xc1: struct.pack('<13h',*([0]*13)), 0xfe: b''}[command]
        self.buffer += frame() + reply(command, data, status=int(command == self.reject))

    def read(self, count):
        result, self.buffer = self.buffer[:min(count,7)], self.buffer[min(count,7):]
        return result


class ProtocolTests(unittest.TestCase):
    def test_queries_parse_interleaved_measurements_and_exit_without_persistent_writes(self):
        uart = DiagnosticUART()
        result = read_diagnostics(uart)
        self.assertEqual(result['firmware'], 'V2.04.23101915')
        self.assertEqual(result['tracking_mode'], 'multiple')
        self.assertEqual(result['zone_filter'], 'disabled')
        self.assertEqual(uart.commands, [0xff,0xa0,0x91,0xc1,0xfe])

    def test_rejected_query_still_leaves_configuration_mode(self):
        uart = DiagnosticUART(reject=0xa0)
        self.assertIn('rejected', read_diagnostics(uart)['query_error'])
        self.assertEqual(uart.commands[-1],0xfe)

    def test_reply_parser_resynchronizes_after_bad_length_and_footer(self):
        parser = ReplyParser()
        malformed = COMMAND_HEADER+b'\xff\xffjunk'+reply(0x91)[:-4]+b'xxxx'
        self.assertEqual(parser.feed(malformed+reply(0xfe)),[(0x1fe,0,b'')])
        parser.feed(b'x'*100000)
        self.assertLessEqual(len(parser.buffer),3)

    def test_wide_angle_target_kept_but_radial_out_of_range_rejected(self):
        self.assertEqual(len(decode_frame(frame(x=4000,y=3000))),1)
        self.assertEqual(decode_frame(frame(x=5000,y=5000)),[])
        self.assertEqual(decode_frame(frame(x=0,y=0)),[])


class PositionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)/'mount.json'
        self.radar = RadarService(config_path=self.path)

    def populate(self):
        tracker = RadarTracker()
        now = time.monotonic()
        for offset in (-.2,-.1,0):
            self.radar.ingest(frame(x=500,y=2000,speed=0),tracker,now+offset)

    def test_rotation_translation_and_mirror(self):
        config = dict(DEFAULT_CONFIG, yaw_deg=90, offset_right_m=.2, offset_forward_m=.3)
        right, forward = sensor_to_drone(1,2,config)
        self.assertAlmostEqual(right,2.2)
        self.assertAlmostEqual(forward,-.7)
        config['invert_x'] = True
        self.assertAlmostEqual(sensor_to_drone(1,2,config)[1],1.3)

    def test_mount_validation_rejects_invalid_numbers_and_tilt(self):
        for update in ({'yaw_deg':float('nan')},{'offset_right_m':True},{'min_range_m':7},
                       {'mounting_confirmed':True,'mount_level':False},{'extra':1}):
            with self.subTest(update=update), self.assertRaises(ValueError):
                validate_config(dict(DEFAULT_CONFIG,**update))

    def test_mount_persists_and_positions_are_independent_of_camera(self):
        config = dict(DEFAULT_CONFIG,mount_level=True,mounting_confirmed=True)
        self.radar.set_config(config)
        self.assertEqual(RadarService(config_path=self.path).config,config)
        self.populate()
        pipeline = CameraPipeline(type('UnusedDetector',(),{'cfg':{'model':'test'}})())
        pipeline.radar = self.radar
        client = create_app(pipeline).test_client()
        self.assertEqual(client.get('/healthz').status_code,503)
        position = client.get('/radar/targets').json
        self.assertEqual(position['status'],'live')
        self.assertEqual(position['reference_origin'],'radar')
        self.assertEqual(position['targets'][0]['drone_position_m'],{'right':.5,'forward':2.})
        packet = make_packet(pipeline.snapshot(),None,'waiting')
        self.assertEqual(packet['detections'],[])
        self.assertEqual(len(packet['drone_relative_radar_targets']),1)
        self.assertIsNone(packet['drone_relative_radar_targets'][0]['confidence'])
        self.radar.last_frame -= 1
        self.assertEqual(client.get('/radar/targets').json['raw_targets'],[])
        self.assertEqual(make_packet(pipeline.snapshot(),None,'waiting')['drone_relative_radar_targets'],[])

    def test_unconfirmed_mount_withholds_drone_position_but_keeps_raw_data(self):
        self.populate()
        state = self.radar.snapshot()
        self.assertIsNone(state['targets'][0]['drone_position_m'])
        self.assertEqual(len(state['raw_targets']),1)
        self.assertFalse(state['position_accuracy_verified'])

    def test_range_filter_does_not_suppress_raw_diagnostics(self):
        self.radar.set_config(dict(DEFAULT_CONFIG,min_range_m=3))
        self.populate()
        state = self.radar.snapshot()
        self.assertEqual(state['targets'],[])
        self.assertEqual(len(state['raw_targets']),1)

    def test_standalone_uses_shared_routes_without_opening_hardware_on_import(self):
        path = Path(__file__).resolve().parents[2]/'Radar'/'ld2450_radar.py'
        spec = importlib.util.spec_from_file_location('standalone_radar',path)
        module = importlib.util.module_from_spec(spec)
        with patch.object(RadarService,'start') as start:
            spec.loader.exec_module(module)
            start.assert_not_called()
        self.populate()
        client = module.create_app(self.radar).test_client()
        self.assertEqual(client.get('/').location,'/radar')
        self.assertEqual(client.get('/targets').json['targets'][0]['x_m'],.5)
        self.assertEqual(client.get('/radar/targets').json['targets'][0]['right_m'],.5)
        before = dict(self.radar.config)
        self.assertEqual(client.post('/radar/config',json={'yaw_deg':45}).status_code,400)
        self.assertEqual(self.radar.config,before)
        self.assertEqual(client.post('/radar/trials',json={'label':'invalid'}).status_code,400)

    def test_crossing_slot_order_does_not_assign_same_track_twice(self):
        tracker = RadarTracker()
        left={'x':-1000,'y':2000,'spd':0}
        right={'x':1000,'y':2000,'spd':0}
        for n in range(3):
            tracks = tracker.update([left,right],1+n*.1)
        original={t['x']:t['id'] for t in tracks}
        reordered = tracker.update([right,left],1.3)
        self.assertEqual({t['x']:t['id'] for t in reordered},original)
        self.assertEqual(tracker.update([],1.4),[])


class TrialTests(unittest.TestCase):
    def test_no_data_is_not_a_successful_empty_baseline(self):
        result = summarize([],outages=20)
        self.assertIsNone(result['target_frame_fraction'])
        self.assertIsNone(result['mean_position_error_m'])
        self.assertEqual(result['outage_polls'],20)

    def test_error_uses_only_unambiguous_frames_and_reports_detection_fraction(self):
        target={'right_m':.3,'forward_m':2.4}
        rows=[{'targets':[]},{'targets':[target]},{'targets':[target,target]}]
        result = summarize(rows,0,2)
        self.assertEqual(result['target_frame_fraction'],.6667)
        self.assertEqual(result['mean_position_error_m'],.5)
        self.assertEqual(result['position_samples'],1)
        self.assertEqual(result['ambiguous_frames'],1)
        self.assertFalse(result['through_wall_verified'])

    def test_trials_validate_labels_coordinates_duration_and_busy(self):
        with tempfile.TemporaryDirectory() as directory:
            trials = RadarTrials(None,directory)
            for arguments in ({'label':'invented'},{'label':'empty','right_m':0,'forward_m':2},
                              {'label':'person_clear','right_m':1},{'label':'empty','duration_s':float('nan')}):
                with self.subTest(arguments=arguments),self.assertRaises(ValueError):
                    trials.start(**arguments)
            trials.current={'status':'recording'}
            with self.assertRaises(ValueError):
                trials.start('empty')

    def test_record_counts_unique_frames_and_restores_report_after_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            snapshot = {'config':{},'status':'live','frame_id':1,'targets':[],'raw_targets':[]}
            radar=type('FakeRadar',(),{'snapshot':lambda self:snapshot})()
            trials=RadarTrials(radar,directory)
            clock=[0.]
            def advance(seconds):
                clock[0]+=seconds
                if clock[0]>5.2:
                    snapshot['frame_id']=2
            with patch('radar_trials.time.sleep',side_effect=advance),patch('radar_trials.time.monotonic',side_effect=lambda:clock[0]):
                trials.record('fixture','empty',None,None,.5)
            self.assertEqual(trials.snapshot()['current']['summary']['frames'],2)
            restored=RadarTrials(radar,directory).snapshot()['reports']
            self.assertEqual(restored[0]['summary']['target_frame_fraction'],0.)


if __name__ == '__main__':
    unittest.main()
