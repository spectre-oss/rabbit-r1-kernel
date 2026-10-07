# SPDX-License-Identifier: GPL-2.0-only
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class ReleaseGate(unittest.TestCase):
    def run_gate(self, change=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script = root / 'scripts/rabbit-r1/release-gate.py'
            script.parent.mkdir(parents=True)
            script.write_bytes((ROOT / 'scripts/rabbit-r1/release-gate.py').read_bytes())
            status = json.loads((ROOT / 'Documentation/rabbit-r1/release-status.json').read_text())
            status['publication_ready'] = True
            status['checks'] = {key: 'pass' for key in status['checks']}
            if change:
                change(status)
            path = root / 'Documentation/rabbit-r1/release-status.json'
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(status))
            return subprocess.run([sys.executable, str(script)], capture_output=True, text=True)

    def test_complete_record_passes(self):
        self.assertEqual(self.run_gate().returncode, 0)

    def test_each_private_input_gate_blocks(self):
        for key in ['panel_table_redistribution', 'wlan_input_redistribution',
                    'camera_receiver_input_provenance', 'encoder_mode_input_redistribution']:
            with self.subTest(key=key):
                result = self.run_gate(lambda s: s['checks'].update({key: 'unresolved'}))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(key, result.stderr)

    def test_missing_input_gate_blocks(self):
        result = self.run_gate(lambda s: s['checks'].pop('wlan_input_redistribution'))
        self.assertNotEqual(result.returncode, 0)

    def test_publication_flag_required(self):
        self.assertNotEqual(self.run_gate(lambda s: s.update(publication_ready=False)).returncode, 0)
