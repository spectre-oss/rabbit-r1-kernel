import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('gpu_extract', ROOT / 'patches/rabbit-r1/extract-gpu.py')
gpu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpu)

class GpuExtraction(unittest.TestCase):
    def test_output_guards(self):
        for target in [Path('relative'), ROOT / 'private-blobs']:
            with self.assertRaises(ValueError):
                gpu.validate_output(target)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                gpu.validate_output(Path(tmp))
            link = Path(tmp) / 'alias'
            link.symlink_to(Path(tmp) / 'absent')
            with self.assertRaises(ValueError):
                gpu.validate_output(link)

    def exercise(self, mismatch=False, failure=False):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'vendor.img'
            image.write_bytes(b'unchanged image')
            out = Path(tmp) / 'result'
            manifest = {'reference': 'synthetic', 'files': [{'partition': 'vendor', 'source': '/firmware/rgx.fw',
                'sha256': hashlib.sha256(b'expected').hexdigest()}]}
            def runner(argv, **kwargs):
                self.assertNotIn('-w', argv)
                self.assertEqual(argv[:3], ['debugfs', '-R', 'dump /firmware/rgx.fw payload'])
                if failure:
                    raise OSError('synthetic tool failure')
                (kwargs['cwd'] / 'payload').write_bytes(b'wrong' if mismatch else b'expected')
            # Extraction fixture is under the test checkout's temp environment;
            # output-location policy is tested independently above.
            with patch.object(gpu, 'validate_output', return_value=out):
                if mismatch or failure:
                    with self.assertRaises((ValueError, OSError)):
                        gpu.extract({'vendor': image}, out, manifest, runner)
                    self.assertFalse(out.exists())
                else:
                    result = gpu.extract({'vendor': image}, out, manifest, runner)
                    self.assertEqual(len(result), 1)
                    blob = out / 'vendor/firmware/rgx.fw'
                    self.assertEqual(blob.read_bytes(), b'expected')
                    self.assertEqual(blob.stat().st_mode & 0o777, 0o600)
            self.assertEqual(image.read_bytes(), b'unchanged image')

    def test_verified_read_only_extraction(self):
        self.exercise()

    def test_hash_failure_removes_partial_output(self):
        self.exercise(mismatch=True)

    def test_tool_failure_removes_partial_output(self):
        self.exercise(failure=True)
