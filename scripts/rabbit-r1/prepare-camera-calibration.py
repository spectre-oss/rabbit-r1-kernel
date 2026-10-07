#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build receiver settings from private port-2 calibration; never accesses hardware."""
import json
from pathlib import Path

def render(template, words):
 if type(words)!=list or len(words)!=2 or any(type(x)!=int or not 0<x<=0xffffffff for x in words):
  raise ValueError('Two nonzero u32 calibration words required for the verified port-2 path')
 values={};out=[]
 for row in template:
  address=row['address'];keep=row['keep']
  if 'calibration' in row:
   word,shift,destination=row['calibration'];mask=31<<destination
   value=(values.get(address,0)&~mask)|(((words[word]>>shift)&31)<<destination)
   values[address]=value
  else:value=row['value']
  out.append((address,keep,value))
 return out

def main():
 import argparse,hashlib,os
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('calibration',type=Path)
 parser.add_argument('output_directory',type=Path)
 parser.add_argument('--expected-sha256',required=True)
 args=parser.parse_args()
 root=Path(__file__).resolve().parents[2]
 try:
  source=args.calibration
  if not source.is_absolute() or source.is_symlink() or source.resolve().is_relative_to(root):
   raise ValueError('Calibration must be an absolute external non-symlink file')
  with source.open('rb') as f: raw=f.read(4097)
  if len(raw)>4096 or hashlib.sha256(raw).hexdigest()!=args.expected_sha256:
   raise ValueError('Calibration size or hash mismatch')
  data=json.loads(raw)
  if type(data)!=dict or set(data)!={'port2_words'}:
   raise ValueError('Expected only port2_words')
  template=json.loads(Path(__file__).with_name('camera-receiver-template.json').read_text())
  rows=render(template,data['port2_words'])
  output=args.output_directory/'include/generated/rabbit-r1-camera-receiver.h'
  if not output.is_absolute() or output.resolve().is_relative_to(root):
   raise ValueError('Output must be absolute and outside source tree')
  text=['/* Private generated build input; do not publish. */',
        'struct receiver_step {u32 physical,keep,value;};',
        'static const struct receiver_step receiver_steps[] = {']
  text+=[' {0x%08x,0x%08x,0x%08x},'%row for row in rows]
  text+=['};','']
  output.parent.mkdir(parents=True,exist_ok=True)
  fd=os.open(output,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
  with os.fdopen(fd,'w') as f:f.write('\n'.join(text))
 except (OSError,ValueError,TypeError) as error:parser.error(str(error))
 print('Prepared receiver header; generated calibration remains private.')

if __name__=='__main__':main()
