import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from radar_service import RadarTracker


class RadarMotionTests(unittest.TestCase):
    def test_crossing_tracks_follow_velocity_instead_of_uart_slot_order(self):
        tracker=RadarTracker();initial=None
        for i in range(14):
            a=dict(x=-650+i*100,y=2000,spd=0,slot=i%2)
            b=dict(x=650-i*100,y=2220,spd=0,slot=(i+1)%2)
            result=tracker.update([a,b] if i%2 else [b,a],i*.1)
            if i==3:
                initial={r['raw_y']:r['id'] for r in result}
        self.assertEqual({r['raw_y']:r['id'] for r in result},initial)
        right=next(r for r in result if r['raw_y']==2000)
        self.assertGreater(right['sensor_velocity_x_mps'],.8)

    def test_single_teleport_cannot_create_a_confirmed_figure(self):
        tracker=RadarTracker()
        for i in range(3):tracker.update([dict(x=0,y=2000,spd=0)],i*.1)
        self.assertEqual(tracker.update([dict(x=3000,y=2000,spd=0)],.3),[])
        restored=tracker.update([dict(x=0,y=2000,spd=0)],.4)
        self.assertEqual(len(restored),1)
        self.assertEqual(restored[0]['id'],1)

    def test_duplicate_read_cannot_confirm_and_expired_identity_is_not_reused(self):
        tracker=RadarTracker();d=[dict(x=0,y=2000,spd=0)]
        for _ in range(5):self.assertEqual(tracker.update(d,1),[])
        tracker.update(d,1.1)
        first=tracker.update(d,1.2)[0]['id']
        self.assertEqual(tracker.update(d,2),[])
        tracker.update(d,2.1)
        self.assertNotEqual(tracker.update(d,2.2)[0]['id'],first)
