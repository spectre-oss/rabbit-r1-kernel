#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only inventory for private source/artifact correspondence checks.

Run on the device and capture stdout privately on the maintainer host. Does not
emit hostname, serial, boot UUID, network addresses, command line, or credentials.
No service or device-control operations are performed. Module filenames/hashes
are development evidence; review before making an inventory public.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import struct


def digest(data):
    return hashlib.sha256(data).hexdigest()


def elf_metadata(raw):
    if raw[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected little-endian ELF64 module')
    offset = struct.unpack_from('<Q', raw, 40)[0]
    size, count, strings = struct.unpack_from('<HHH', raw, 58)
    if size != 64 or count > 65535 or offset + count * size > len(raw) or strings >= count:
        raise ValueError('Invalid ELF section table')
    def section(index):
        return struct.unpack_from('<IIQQQQIIQQ', raw, offset + index * size)
    entry = section(strings)
    names = raw[entry[4]:entry[4] + entry[5]]
    result = {}
    for index in range(count):
        entry = section(index)
        name = names[entry[0]:].split(b'\0', 1)[0]
        data = raw[entry[4]:entry[4] + entry[5]]
        if name == b'.modinfo':
            for item in data.split(b'\0'):
                key, _, value = item.partition(b'=')
                if key in (b'name', b'vermagic', b'license', b'srcversion', b'depends'):
                    result[key.decode()] = value.decode(errors='replace')
        elif name == b'.note.gnu.build-id':
            result['build_note_sha256'] = digest(data)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extra-root', action='append', type=Path, default=[],
                        help='Additional known module directory to inventory read-only')
    args = parser.parse_args()
    uname = os.uname()
    result = {'schema_version': 1, 'kernel_release': uname.release,
              'kernel_version': uname.version, 'architecture': uname.machine,
              'loaded_modules': [], 'installed_module_files': [], 'errors': []}
    config = Path('/proc/config.gz')
    if config.exists():
        result['kernel_config_sha256'] = digest(gzip.decompress(config.read_bytes()))
    for line in Path('/proc/modules').read_text().splitlines():
        name = line.split()[0]
        row = {'name': name}
        note = Path('/sys/module') / name / 'notes/.note.gnu.build-id'
        if note.exists():
            row['build_note_sha256'] = digest(note.read_bytes())
        result['loaded_modules'].append(row)
    roots = list(Path('/usr/lib').glob('r1-*'))
    roots += [Path('/usr/lib/spectrer1'), Path('/lib/modules') / uname.release]
    roots += args.extra_root
    seen = set()
    for root in roots:
        if not root.is_dir():
            continue
        for directory, subdirs, files in os.walk(root, followlinks=False):
            subdirs[:] = [name for name in subdirs if not name.startswith(('before', 'backup', '.'))]
            for name in sorted(files):
                if not name.endswith('.ko'):
                    continue
                path = Path(directory) / name
                real = path.resolve()
                if real in seen:
                    continue
                seen.add(real)
                try:
                    raw = path.read_bytes()
                    result['installed_module_files'].append({'path': str(path), 'sha256': digest(raw), **elf_metadata(raw)})
                except (OSError, ValueError, struct.error) as error:
                    result['errors'].append({'path': str(path), 'error_type': type(error).__name__})
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
