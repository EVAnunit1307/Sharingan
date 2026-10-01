import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import zipfile

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'SensorRig'/'CV'))
from camera_dashboard import CameraPipeline, create_app as pi_app
from mapping_capture import MappingCapture
from Mapping.server import create_app, import_archive
from Mapping.reconstruct import select_model
from Mapping.keyframes import sharpest_windows


class ReconstructionTests(unittest.TestCase):
    def test_keyframes_preserve_endpoints_and_low_texture_intervals(self):
        rows=[{'host_capture_mono_ns':i*100_000_000,'sharpness':score} for i,score in enumerate([1,2,12,0,0,0,0])]
        original=json.dumps(rows)
        selected=sharpest_windows(rows,window_seconds=.3)
        self.assertIn(0,selected)
        self.assertIn(2,selected)
        self.assertIn(3,selected)  # A blank interval is retained, not silently dropped.
        self.assertIn(6,selected)
        self.assertEqual(original,json.dumps(rows))

    def test_component_with_more_cameras_but_no_geometry_does_not_hide_usable_model(self):
        class Model:
            def __init__(self, cameras, points): self.cameras, self.points = cameras, points
            def num_reg_images(self): return self.cameras
            def num_points3D(self): return self.points
        empty, usable = Model(21, 1), Model(15, 60)
        self.assertIs(select_model({0:empty, 1:usable}), usable)
        self.assertIsNone(select_model({0:empty, 1:Model(2, 200)}))


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.capture = MappingCapture(self.root, fps=10, min_free_bytes=0)
        self.pipeline = CameraPipeline(None, mapping=self.capture)
        self.client = pi_app(self.pipeline).test_client()

    def tearDown(self):
        self.capture.close()
        self.temporary.cleanup()

    def publish(self, seq=1, generation=1):
        image = np.zeros((48, 64, 3), dtype=np.uint8)
        image[:, :, 1] = 180
        self.capture.submit(image, seq, time.monotonic()+seq*.11, generation,
                            {'SensorTimestamp':123456789, 'ExposureTime':1000}, 180)
        self.capture.jobs.join()
        with self.capture.condition:
            self.assertTrue(self.capture.condition.wait_for(lambda: self.capture.preview_sequence == seq, timeout=2))

    def test_clean_images_and_sensor_metadata_survive_stop_download_import(self):
        self.assertEqual(self.client.post('/mapping/start').status_code, 200)
        self.publish()
        session_id=self.capture.active['manifest']['session_id']
        self.assertEqual(self.client.get(f'/mapping/sessions/{session_id}/archive').status_code,409)
        self.assertEqual(self.client.post('/mapping/stop').status_code,200)
        manifest=json.loads((self.root/session_id/'manifest.json').read_text())
        self.assertEqual(manifest['status'],'complete')
        self.assertEqual(manifest['frames_saved'],1)
        row=json.loads((self.root/session_id/'frames.jsonl').read_text())
        self.assertEqual(row['sensor_timestamp_ns'],123456789)
        self.assertEqual(row['exposure_us'],1000)
        self.assertNotEqual(row['host_capture_mono_ns'],row['sensor_timestamp_ns'])
        image=cv2.imread(str(self.root/session_id/row['image']))
        self.assertGreater(image[:,:,1].mean(),175)
        self.assertLess(image[:,:,2].mean(),5)
        with self.client.get(f'/mapping/sessions/{session_id}/archive') as response:
            archive=io.BytesIO(response.data)
        destination=import_archive(archive,self.root/'laptop',session_id)
        self.assertTrue((destination/row['image']).exists())
        self.assertEqual(json.loads((destination/'manifest.json').read_text())['scale'],'unknown')

    def test_burst_selects_sharp_frame_with_its_own_metadata_and_flushes_on_stop(self):
        self.capture.configure(selection='sharpest',exposure_us=10000,gain=8)
        self.capture.settings_changed_at-=2
        sid=self.capture.start()['session_id']
        tile=np.indices((120,160)).sum(axis=0)//4%2*255
        sharp=np.repeat(tile[:,:,None],3,axis=2).astype(np.uint8)
        blur=cv2.GaussianBlur(sharp,(15,15),4)
        origin=time.monotonic()
        for index,(offset,frame) in enumerate([(0,blur),(.04,sharp),(.08,blur),(.12,blur),(.16,sharp)]):
            self.capture.submit(frame,index+1,origin+offset,1,{'SensorTimestamp':1000+index,'ExposureTime':10000})
        self.capture.stop()
        rows=[json.loads(line) for line in (self.root/sid/'frames.jsonl').read_text().splitlines()]
        self.assertEqual([row['frame_id'] for row in rows],[2,5])
        self.assertEqual([row['sensor_timestamp_ns'] for row in rows],[1001,1004])
        self.assertEqual([row['selection_candidates'] for row in rows],[3,2])
        self.assertEqual(json.loads((self.root/sid/'manifest.json').read_text())['frame_selection'],'sharpest')

    def test_capture_settings_cannot_change_mid_recording_and_reconnect_flushes_old_burst(self):
        self.capture.configure(selection='sharpest');self.capture.settings_changed_at-=2
        sid=self.capture.start()['session_id'];self.publish(1,1)
        with self.assertRaises(ValueError):self.capture.configure(exposure_us=10000)
        self.publish(2,2);self.capture.jobs.join()
        rows=[json.loads(line) for line in (self.root/sid/'frames.jsonl').read_text().splitlines()]
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['camera_generation'],1)
        self.assertEqual(self.capture.sessions()[0]['stop_reason'],'camera_reconnected')

    def test_capture_settings_validate_ranges_and_have_settling_period(self):
        for options in ({'selection':'bad'},{'exposure_us':-1},{'exposure_us':float('nan')},{'gain':17}):
            with self.assertRaises(ValueError):self.capture.configure(**options)
        self.capture.configure(exposure_us=10000,gain=8)
        with self.assertRaises(ValueError):self.capture.start()
        _,controls=self.capture.camera_controls()
        self.assertFalse(controls['AeEnable']);self.assertEqual(controls['ExposureTime'],10000)
        self.capture.configure();self.assertEqual(self.capture.camera_controls()[1],{'AeEnable':True})

    def test_rate_change_validated_by_http_and_locked_during_capture(self):
        for bad in (0, -1, 25, True, '12', float('nan'), float('inf')):
            self.assertEqual(self.client.post('/mapping/configure',json={'fps':bad}).status_code,409)
        reply=self.client.post('/mapping/configure',json={'fps':12})
        self.assertEqual(reply.status_code,200)
        self.assertEqual(reply.json['sample_fps'],12)
        self.capture.settings_changed_at-=2
        sid=self.capture.start()['session_id']
        self.assertEqual(self.client.post('/mapping/configure',json={'fps':24}).status_code,409)
        self.assertEqual(self.capture.fps,12)
        self.capture.stop()
        self.assertEqual(json.loads((self.root/sid/'manifest.json').read_text())['sample_fps'],12)

    def test_uniform_rate_keeps_sampling_clock_through_jitter_and_gaps(self):
        self.capture.configure(fps=12);self.capture.settings_changed_at-=2
        sid=self.capture.start()['session_id']
        frame=np.full((48,64,3),80,dtype=np.uint8)
        origin=time.monotonic()
        # 24 sensor fps, with arrival jitter that previously reduced the saved
        # rate by waiting a full new interval after every selected frame.
        for i in range(240):
            when=origin+i/24+(0 if i%2 else .0007)
            self.capture.submit(frame,i,when,1,{'SensorTimestamp':round(when*1e9)})
            self.capture.jobs.join()
        before=self.capture.status()['frames_saved']
        self.assertTrue(119<=before<=121,before)
        self.capture.submit(frame,300,origin+20,1,{'SensorTimestamp':round((origin+20)*1e9)})
        self.capture.jobs.join()
        self.assertEqual(self.capture.status()['frames_saved'],before+1)
        self.capture.stop()
        rows=[json.loads(line) for line in (self.root/sid/'frames.jsonl').read_text().splitlines()]
        self.assertEqual(len({r['frame_id'] for r in rows}),len(rows))
        self.assertTrue(all(b['sensor_timestamp_ns']>a['sensor_timestamp_ns'] for a,b in zip(rows,rows[1:])))

    def test_full_sensor_rate_does_not_discard_alternate_early_arrivals(self):
        self.capture.configure(fps=24);self.capture.settings_changed_at-=2
        self.capture.start()
        frame=np.full((48,64,3),80,dtype=np.uint8);origin=time.monotonic()
        for i in range(48):
            self.capture.submit(frame,i,origin+i/24+(0 if i%2 else .0007),1)
            self.capture.jobs.join()
        self.assertEqual(self.capture.status()['frames_saved'],48)

    def test_reconnect_closes_recording_without_mixing_generations(self):
        session_id=self.capture.start()['session_id']
        self.publish(1,1)
        self.publish(2,2)
        self.assertIsNone(self.capture.active)
        manifest=json.loads((self.root/session_id/'manifest.json').read_text())
        self.assertEqual(manifest['frames_saved'],1)
        self.assertEqual(manifest['stop_reason'],'camera_reconnected')

    def test_autostart_and_time_limit(self):
        self.capture.autostart=True
        self.publish()
        self.assertIsNotNone(self.capture.active)
        self.capture.active['started']-=121
        self.publish(2)
        self.assertIsNone(self.capture.active)
        self.assertEqual(self.capture.sessions()[0]['stop_reason'],'time_limit')

    def test_failed_disk_write_does_not_break_camera_or_claim_success(self):
        session_id=self.capture.start()['session_id']
        with patch('mapping_capture.Path.write_bytes',side_effect=OSError('disk test failure')):
            self.publish()
        self.assertIsNone(self.capture.active)
        self.assertEqual(self.capture.sessions()[0]['status'],'failed')
        self.assertIn('disk test failure',self.capture.status()['error'])
        self.capture.start()
        self.publish(2)
        self.assertEqual(self.capture.status()['frames_saved'],1)

    def test_old_recording_is_recoverable_after_process_restart(self):
        session_id=self.capture.start()['session_id']
        self.publish()
        self.capture.stop()
        path=self.root/session_id/'manifest.json'
        manifest=json.loads(path.read_text());manifest['status']='recording';path.write_text(json.dumps(manifest))
        self.assertEqual(self.capture.sessions()[0]['status'],'interrupted')
        destination=import_archive(self.capture.archive(session_id),self.root/'recovery',session_id)
        self.assertEqual(json.loads((destination/'manifest.json').read_text())['status'],'interrupted')

    def test_two_starts_never_overwrite_and_foreign_origin_cannot_control_capture(self):
        self.capture.start()
        self.assertEqual(self.client.post('/mapping/start').status_code,409)
        self.assertEqual(self.client.post('/mapping/stop',headers={'Origin':'http://elsewhere'}).status_code,403)
        self.assertEqual(len(list(self.root.glob('*/manifest.json'))),1)
        self.capture.stop()
        self.capture.start()
        self.assertEqual(len(list(self.root.glob('*/manifest.json'))),2)

    def test_stale_preview_is_not_served(self):
        self.publish()
        self.assertEqual(self.client.get('/mapping/preview.jpg').status_code,200)
        with self.capture.lock:
            when,jpeg,sharp=self.capture.preview
            self.capture.preview=(when-10,jpeg,sharp)
        self.assertEqual(self.client.get('/mapping/preview.jpg').status_code,503)

    def test_pipeline_passes_clean_image_to_recorder_without_detector(self):
        self.capture.start()
        self.pipeline.publish_frame(np.full((48,64,3),80,dtype=np.uint8),time.monotonic(),4,
                                    {'SensorTimestamp':500})
        self.capture.jobs.join()
        self.assertEqual(self.capture.status()['frames_saved'],1)
        self.assertEqual(self.pipeline.snapshot()['model'],'disabled')

    def test_preview_is_faster_than_recording_and_does_not_save_extra_images(self):
        self.capture.fps = 1
        self.capture.start()
        self.publish(1)
        self.publish(2)
        self.assertEqual(self.capture.status()['frames_saved'], 1)
        self.assertEqual(self.capture.preview_sequence, 2)
        self.assertEqual(self.client.get('/mapping/preview.jpg').headers['X-Frame-Id'], '2')

    def test_preview_keeps_up_when_recording_disk_is_blocked(self):
        entered, release = threading.Event(), threading.Event()
        original = Path.write_bytes
        def slow_write(path, data):
            entered.set()
            release.wait(3)
            return original(path, data)
        self.capture.start()
        try:
            with patch('mapping_capture.Path.write_bytes', slow_write):
                frame = np.full((48,64,3), 80, dtype=np.uint8)
                self.capture.submit(frame, 1, time.monotonic(), 1)
                self.assertTrue(entered.wait(2))
                self.capture.submit(frame, 2, time.monotonic(), 1)
                with self.capture.condition:
                    self.assertTrue(self.capture.condition.wait_for(lambda: self.capture.preview_sequence == 2, timeout=2))
                self.assertEqual(self.capture.status()['frames_saved'], 0)
                release.set()
                self.capture.jobs.join()
        finally:
            release.set()

    def test_slow_stream_consumer_skips_old_frames_and_stale_stream_ends(self):
        self.publish(1)
        stream = self.capture.preview_stream()
        self.assertIn(b'X-Frame-Id: 1\r\n', next(stream))
        self.publish(2)
        self.publish(3)
        self.assertIn(b'X-Frame-Id: 3\r\n', next(stream))
        with self.capture.lock:
            when,jpeg,sharp = self.capture.preview
            self.capture.preview = (when-10,jpeg,sharp)
        with self.assertRaises(StopIteration):
            next(stream)


class LaptopTests(unittest.TestCase):
    def test_build_routes_explicit_backend_and_rejects_unknown_backend(self):
        from Mapping.server import MapJobs
        with tempfile.TemporaryDirectory() as root:
            client=create_app(None,Path(root)).test_client()
            session='20260925T120000Z-1234abcd'
            with patch.object(MapJobs,'start') as start:
                response=client.post('/map-api/build/'+session,json={'backend':'xfeat'})
                self.assertEqual(response.status_code,202)
                start.assert_called_once_with(session,'xfeat')
            self.assertEqual(client.post('/map-api/build/'+session,json={'backend':'unknown'}).status_code,409)

    def test_local_pi_http_uses_ipv4_without_changing_https_identity(self):
        with tempfile.TemporaryDirectory() as root, patch('Mapping.server.socket.gethostbyname', return_value='192.0.2.4'):
            client = create_app('http://pi.local:8766', Path(root)).test_client()
            self.assertEqual(client.get('/map-api/local').json['pi_http'], 'http://192.0.2.4:8766')
            secure = create_app('https://pi.local:8766', Path(root)).test_client()
            self.assertEqual(secure.get('/map-api/local').json['pi_http'], 'https://pi.local:8766')

    def test_stream_proxy_forwards_chunks_without_waiting_for_full_buffer(self):
        upstream = unittest.mock.MagicMock()
        upstream.headers = {'Content-Type':'multipart/x-mixed-replace; boundary=frame'}
        upstream.read1.side_effect = [b'first-small-frame', b'second-small-frame', b'']
        with tempfile.TemporaryDirectory() as root, patch('Mapping.server.urlopen', return_value=upstream):
            client = create_app('http://pi:8766', Path(root)).test_client()
            with client.get('/map-api/pi/stream', buffered=False) as response:
                iterator = iter(response.response)
                self.assertEqual(next(iterator), b'first-small-frame')
                self.assertEqual(upstream.read1.call_count, 1)
                self.assertEqual(next(iterator), b'second-small-frame')
            upstream.close.assert_called()
            upstream.read.assert_not_called()

    def test_dashboard_and_saved_maps_work_without_pi(self):
        with tempfile.TemporaryDirectory() as root:
            client=create_app(None,Path(root)).test_client()
            with client.get('/map') as response:
                self.assertEqual(response.status_code,200)
                self.assertIn(b'No headset needed',response.data)
            self.assertEqual(client.get('/map-api/pi/status').status_code,503)
            self.assertEqual(client.get('/map-api/local').json['sessions'],[])
            self.assertEqual(client.post('/map-api/pi/start',headers={'Origin':'http://foreign'}).status_code,403)
            self.assertEqual(client.get('/map-api/pi/start').status_code,405)
            self.assertEqual(client.post('/map-api/build/invalid').status_code,409)

    def test_hostile_archive_cannot_escape_import_directory(self):
        with tempfile.TemporaryDirectory() as root:
            for name in ['../escaped','images/../../escaped','/tmp/escaped','images/not-an-image.txt']:
                stream=io.BytesIO()
                with zipfile.ZipFile(stream,'w') as archive:
                    archive.writestr(name,'no')
                stream.seek(0)
                with self.assertRaises(ValueError):
                    import_archive(stream,Path(root),'20260925T120000Z-1234abcd')
            self.assertFalse((Path(root)/'escaped').exists())

    def test_archive_rejects_metadata_image_mismatch(self):
        with tempfile.TemporaryDirectory() as root:
            stream=io.BytesIO();session='20260925T120000Z-1234abcd'
            with zipfile.ZipFile(stream,'w') as archive:
                archive.writestr('manifest.json',json.dumps(dict(schema_version=1,session_id=session,status='complete')))
                archive.writestr('frames.jsonl',json.dumps(dict(image='images/000000001.jpg',camera_generation=1)))
            stream.seek(0)
            with self.assertRaises(ValueError):import_archive(stream,Path(root),session)

    def test_offline_model_can_be_loaded_after_server_restart(self):
        with tempfile.TemporaryDirectory() as root:
            session='20260925T120000Z-1234abcd';path=Path(root)/session;path.mkdir()
            (path/'manifest.json').write_text(json.dumps(dict(session_id=session,status='complete',frames_saved=20)))
            (path/'scene.json').write_text(json.dumps(dict(points=[[0,0,0,255,0,0]],scale='unknown')))
            client=create_app(None,Path(root)).test_client()
            self.assertTrue(client.get('/map-api/local').json['sessions'][0]['has_model'])
            with client.get('/map-api/model/'+session) as response:
                self.assertEqual(response.json['scale'],'unknown')


if __name__=='__main__':unittest.main()
