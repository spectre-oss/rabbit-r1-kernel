# SPDX-License-Identifier: GPL-2.0-only
import importlib.util,json,unittest
from pathlib import Path
BASE=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('calibration',BASE/'prepare-camera-calibration.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Calibration(unittest.TestCase):
 def test_synthetic_fields(self):
  template=json.loads((BASE/'camera-receiver-template.json').read_text())
  for words in [[0xffffffff,0xffffffff],[1,1],[0x12345678,0x87654321]]:
   rows=m.render(template,words);self.assertEqual(len(rows),88)
   for row,source in zip(rows,template):
    if 'calibration' not in source:self.assertEqual(row[2],source['value'])
    else:
     word,shift,dest=source['calibration']
     self.assertEqual((row[2]>>dest)&31,(words[word]>>shift)&31)
 def test_invalid(self):
  for value in [None,{},[1],[0,1],[1,0],[-1,1],[True,1],[1,2**32]]:
   with self.subTest(value=value),self.assertRaises(ValueError):m.render([],value)
