#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate encoder settings from hash-pinned numeric JSON, never C input."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIELDS = {'width', 'height', 'stride_height', 'level', 'sizes', 'control', 'setup', 'headers'}


def integer(value, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError('Numeric input outside allowed bounds')
    return value


def validate(modes):
    if not isinstance(modes, list) or not 1 <= len(modes) <= 16:
        raise ValueError('Expected one to sixteen modes')
    dimensions = set()
    for mode in modes:
        if not isinstance(mode, dict) or set(mode) != FIELDS:
            raise ValueError('Unexpected mode fields')
        width = integer(mode['width'], 16, 4096)
        height = integer(mode['height'], 16, 2160)
        stride = integer(mode['stride_height'], height, 2176)
        if width % 16 or stride % 16 or (width, height) in dimensions:
            raise ValueError('Invalid or duplicate mode geometry')
        dimensions.add((width, height))
        integer(mode['level'], 0, 255)
        integer(mode['control'], 0, 0xffffffff)
        if not isinstance(mode['sizes'], list) or len(mode['sizes']) != 9:
            raise ValueError('Expected nine buffer sizes')
        for size in mode['sizes']:
            integer(size, 1, 64 * 1024 * 1024)
        if not isinstance(mode['setup'], list) or len(mode['setup']) != 24:
            raise ValueError('Expected twenty-four setup writes')
        for pair in mode['setup']:
            if not isinstance(pair, list) or len(pair) != 2:
                raise ValueError('Expected register/value pair')
            if integer(pair[0], 0, 0x1ffc) % 4:
                raise ValueError('Unaligned register')
            integer(pair[1], 0, 0xffffffff)
        if not isinstance(mode['headers'], list) or not 1 <= len(mode['headers']) <= 64:
            raise ValueError('Expected one to sixty-four header bytes')
        for byte in mode['headers']:
            integer(byte, 0, 255)
    return modes


def render(modes):
    validate(modes)
    text = '/* Generated hardware mode data; see encoder-mode-provenance.json. */\n'
    text += ('struct mode { unsigned int width, height, stride_height, level, sizes[9], control; '
             'unsigned int setup[24][2]; unsigned char headers[64]; unsigned int header_size; };\n'
             'static const struct mode modes[] = {\n')
    for mode in modes:
        fields = [f'.{key}={mode[key]}' for key in ('width', 'height', 'stride_height', 'level', 'control')]
        fields.append('.sizes={' + ','.join(map(str, mode['sizes'])) + '}')
        fields.append('.setup={' + ','.join('{' + ','.join(map(str, pair)) + '}' for pair in mode['setup']) + '}')
        fields.append('.headers={' + ','.join(map(str, mode['headers'])) + '}')
        fields.append(f'.header_size={len(mode["headers"])}')
        text += ' {' + ','.join(fields) + '},\n'
    return text + '};\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--expected-sha256', required=True)
    args = parser.parse_args()
    target = (args.output / 'include/generated/rabbit-r1-encoder-modes.h').resolve()
    if args.output.resolve().is_relative_to(ROOT) or target.is_relative_to(ROOT) or target.exists():
        parser.error('Use a new generated header outside the source tree')
    if args.input.stat().st_size > 1024 * 1024:
        parser.error('Input too large')
    raw = args.input.read_bytes()
    if hashlib.sha256(raw).hexdigest() != args.expected_sha256:
        parser.error('Encoder input hash mismatch')
    text = render(json.loads(raw))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)
    target.chmod(0o600)


if __name__ == '__main__':
    main()
