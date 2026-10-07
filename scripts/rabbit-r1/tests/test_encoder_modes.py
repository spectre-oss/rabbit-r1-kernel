# SPDX-License-Identifier: GPL-2.0-only
import copy
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / 'prepare-encoder-modes.py'
spec = importlib.util.spec_from_file_location('encoder_modes', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class EncoderModesTests(unittest.TestCase):
    def mode(self):
        return {'width': 16, 'height': 16, 'stride_height': 16, 'level': 1,
                'sizes': [64] * 9, 'control': 0, 'setup': [[4, 0]] * 24,
                'headers': [0, 1]}

    def test_numeric_render(self):
        self.assertIn('.header_size=2', module.render([self.mode()]))

    def test_reject_code_and_boolean(self):
        for value in ('system("bad")', True, -1, 2**32):
            mode = self.mode(); mode['control'] = value
            with self.assertRaises(ValueError): module.render([mode])

    def test_register_bounds(self):
        for address in (1, 0x2000, -4):
            mode = self.mode(); mode['setup'] = [[address, 0]] * 24
            with self.assertRaises(ValueError): module.render([mode])

    def test_bounded_headers_and_shapes(self):
        for field, value in [('headers', [256]), ('headers', [0]*65), ('sizes', [1]),
                             ('setup', [[4,0]]), ('width', 17), ('stride_height', 15)]:
            mode = self.mode(); mode[field] = value
            with self.assertRaises(ValueError): module.render([mode])

    def test_duplicate_and_extra_fields(self):
        mode = self.mode()
        with self.assertRaises(ValueError): module.render([mode, copy.deepcopy(mode)])
        mode['extra'] = 1
        with self.assertRaises(ValueError): module.render([mode])
