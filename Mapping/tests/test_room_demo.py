import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from Mapping.room_demo import contained, digest, reproduce, verify


class DemoTests(unittest.TestCase):
    def test_bundle_integrity_covers_input_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'demo.json').write_text(json.dumps(dict(frames=0, scenes={})))
            (root / 'image.jpg').write_bytes(b'original camera data')
            (root / 'SHA256SUMS.json').write_text(json.dumps({
                name: digest(root / name) for name in ('demo.json', 'image.jpg')}))
            verify(root)
            (root / 'image.jpg').write_bytes(b'changed image')
            with self.assertRaisesRegex(ValueError, 'Checksum mismatch'):
                verify(root)

    def test_paths_and_symlinks_cannot_escape_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'bundle'
            root.mkdir()
            outside = Path(tmp) / 'outside'
            outside.write_text('untouched')
            (root / 'link').symlink_to(outside)
            for name in ('../outside', 'link', str(outside)):
                with self.assertRaises(ValueError):
                    contained(root, name)

    def test_rerun_preserves_existing_output_before_loading_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            saved = root / 'index.html'
            saved.write_text('existing result')
            with self.assertRaisesRegex(ValueError, 'new output'):
                reproduce(SimpleNamespace(output=root), {})
            self.assertEqual(saved.read_text(), 'existing result')


if __name__ == '__main__':
    unittest.main()
