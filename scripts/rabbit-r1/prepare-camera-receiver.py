#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Parse an explicitly supplied private receiver table into an external build.

No calibration values are bundled. This does not read hardware, execute input
code, or grant redistribution rights for input data or generated artifacts.
"""
import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
NUMBER = r'(?:0x[0-9a-fA-F]+|[0-9]+)'
ROW = re.compile(r'\{\s*(' + NUMBER + r')\s*,\s*(' + NUMBER + r')\s*,\s*(' + NUMBER + r')\s*\}\s*,?')


def extract(source):
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    table = re.fullmatch(r'\s*struct\s+receiver_step\s*\{\s*u32\s+physical\s*,\s*keep\s*,\s*value\s*;\s*\}\s*;\s*'
                         r'static\s+const\s+struct\s+receiver_step\s+receiver_steps\s*\[\s*\]\s*=\s*\{(.*)\}\s*;\s*', text, re.S)
    if not table:
        raise ValueError('Expected only the receiver structure and numeric table')
    body = table[1]; position = 0; rows = []
    for match in ROW.finditer(body):
        if body[position:match.start()].strip():
            raise ValueError('Unexpected content between receiver entries')
        address, keep, value = (int(token, 0) for token in match.groups())
        if address % 4 or not (0x1a040000 <= address < 0x1a048000 or 0x11c10000 <= address < 0x11c16000):
            raise ValueError('Receiver register lies outside the existing mapped windows')
        if not (0 <= keep <= 0xffffffff and 0 <= value <= 0xffffffff):
            raise ValueError('Receiver value exceeds u32')
        rows.append((address, keep, value)); position = match.end()
    if body[position:].strip() or not 1 <= len(rows) <= 512:
        raise ValueError('Invalid receiver table contents or length')
    return rows


def prepare(source, output_directory, expected_sha256):
    output = (output_directory / 'include/generated/rabbit-r1-camera-receiver.h').resolve()
    if output.is_relative_to(ROOT):
        raise ValueError('Private receiver output must stay outside the source tree')
    if not re.fullmatch(r'[0-9a-f]{64}', expected_sha256):
        raise ValueError('Supply the privately recorded expected SHA256')
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError('Private receiver input hash mismatch')
    rows = extract(raw.decode())
    text = ['/* Private generated build input; do not publish. */',
            'struct receiver_step {u32 physical,keep,value;};',
            'static const struct receiver_step receiver_steps[] = {']
    text += [' {0x%08x,0x%08x,0x%08x},' % row for row in rows]
    text += ['};', '']
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(text))
    return len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output_directory', type=Path)
    parser.add_argument('--expected-sha256', required=True)
    args = parser.parse_args()
    try:
        count = prepare(args.source, args.output_directory, args.expected_sha256)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Prepared {count} receiver entries in the private build output.')


if __name__ == '__main__':
    main()
