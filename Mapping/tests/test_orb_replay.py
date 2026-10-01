import csv
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from Mapping.orb_replay import prepare, summarize
from Mapping.server import create_app

SESSION = '20261001T053033Z-f37549ec'

class ReplayTests(unittest.TestCase):
    def write_csv(self, root, name, fields, rows):
        with (root/name).open('w') as file:
            writer = csv.writer(file)
            writer.writerow(fields.split(','))
            writer.writerows(rows)

    def test_lost_frames_hidden_and_disconnected_maps_never_combined(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prepared = dict(session_id=SESSION, fps=3, seconds=[0,1,2,3], calibration_sha256='test',
                            frames=[dict(image=f'images/{i:09}.jpg') for i in range(4)])
            self.write_csv(root,'tracking.csv','index,timestamp,state,map_id,landmarks,processing_ms',
                           [[0,0,2,1,80,20],[1,1,3,1,1,20],[2,2,2,2,80,20],[3,3,2,2,80,20]])
            self.write_csv(root,'poses.csv','timestamp,map_id,x,y,z,qx,qy,qz,qw',
                           [[i,1 if i<2 else 2,i,0,0,0,0,0,1] for i in range(4)])
            self.write_csv(root,'points.csv','map_id,point_id,x,y,z,observations',
                           [[1,0,0,0,1,3],[2,1,50,0,1,3],[2,2,100,0,1,2]])
            result = summarize(root,prepared)
            self.assertEqual(result['retained_poses'],3)
            self.assertEqual(result['longest_contiguous_frames'],2)
            self.assertIsNone(result['frames'][1]['position'])
            self.assertEqual(len(result['maps']),2)
            self.assertEqual(result['maps'][0]['frames'],[2,3])
            self.assertEqual(result['maps'][0]['points'],[[50,0,1]])
            self.assertEqual(result['scale'],'unknown')

    def test_partial_process_cannot_publish_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            self.write_csv(root,'tracking.csv','index,timestamp,state,map_id,landmarks,processing_ms',[])
            with self.assertRaisesRegex(ValueError,'every frame'):
                summarize(root,dict(frames=[{}]))

    def test_prepare_preserves_sensor_timing_and_opencv_pixel_centers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/'images').mkdir()
            for i in range(2):(root/f'images/{i:09}.jpg').write_bytes(b'test')
            (root/'manifest.json').write_text(json.dumps(dict(session_id=SESSION)))
            rows=[dict(image=f'images/{i:09}.jpg',camera_generation=1,sensor_timestamp_ns=1000000000+i*333333333,
                       host_capture_mono_ns=5000000000+i*500000000) for i in range(2)]
            (root/'frames.jsonl').write_text('\n'.join(map(json.dumps,rows)))
            calibration=dict(params=[800,801,300,350,0,0,0,0],width=640,height=480,source_sha256='test')
            with patch('Mapping.orb_replay.load_calibration',return_value=calibration):
                data=prepare(root,root/'output')
                self.assertEqual(data['seconds'],[0,.333333333])
                self.assertIn('Camera1.cx: 300.000000000000',(root/'output/camera.yaml').read_text())
                self.assertIn('Camera.fps: 3\n',(root/'output/camera.yaml').read_text())
                rows[1]['sensor_timestamp_ns']=rows[0]['sensor_timestamp_ns']
                (root/'frames.jsonl').write_text('\n'.join(map(json.dumps,rows)))
                with self.assertRaisesRegex(ValueError,'increase strictly'):prepare(root,root/'invalid')

    def test_recorded_image_routes_limit_paths_and_preserve_existing_map(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);session=root/SESSION;session.mkdir();(session/'images').mkdir()
            (session/'tracking.json').write_text('{"kind":"orb_tracking_replay"}')
            (session/'partial.json').write_text('{"kind":"offline_partial_replay"}')
            (session/'scene.json').write_text('{"kind":"original"}')
            (session/'images/000000001.jpg').write_bytes(b'jpeg')
            client=create_app(None,root).test_client()
            with client.get('/map-api/tracking/'+SESSION) as response:
                self.assertEqual(response.json['kind'],'orb_tracking_replay')
            with client.get('/map-api/partial/'+SESSION) as response:
                self.assertEqual(response.json['kind'],'offline_partial_replay')
            with client.get('/map-api/model/'+SESSION) as response:
                self.assertEqual(response.json['kind'],'original')
            with client.get('/map-api/frame/'+SESSION+'/000000001.jpg') as response:
                self.assertEqual(response.data,b'jpeg')
            self.assertEqual(client.get('/map-api/frame/'+SESSION+'/manifest.json').status_code,400)
            self.assertEqual(client.get('/map-api/tracking/invalid').status_code,400)

if __name__=='__main__':unittest.main()
