#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the reviewed patch payload; vendor source is not needed."""
import hashlib
import json
from pathlib import Path
import subprocess

BUNDLE = Path(__file__).resolve().parent

def main():
    manifest = json.loads((BUNDLE / 'manifest.json').read_text())
    expected = {
        '0001-rabbit-wlan-linux71.patch': {r['source'] for r in manifest['vendor_files'] if r['source_sha256'] != r['prepared_sha256']},
        '0002-spectre-optional-integration.patch': {r['path'] for r in manifest['integration_files']},
    }
    for name, paths in expected.items():
        patch = BUNDLE / name
        data = patch.read_bytes()
        if hashlib.sha256(data).hexdigest() != manifest['patch_sha256'][name]:
            raise ValueError('Patch hash mismatch: ' + name)
        if b'GIT binary patch' in data or b'Binary files ' in data:
            raise ValueError('Binary patches are not permitted')
        stats = subprocess.check_output(['git', 'apply', '--numstat', str(patch)]).decode().splitlines()
        actual = {line.split('\t', 2)[2] for line in stats}
        if actual != paths:
            raise ValueError('Unexpected patch targets: ' + name)
        for path in actual:
            if Path(path).is_absolute() or '..' in Path(path).parts:
                raise ValueError('Unsafe patch path')
            if any(path.endswith('/' + omitted) for omitted in manifest['excluded_vendor_inputs']):
                raise ValueError('Deferred vendor file included in patch')
            if path.endswith(('panel-rabbit-r1-init.h', 'rabbit-r1-panel-init.h')):
                raise ValueError('Generated panel table included in patch')
    print('Patch hashes, target scope, text-only payload and deferred-input exclusions pass.')

if __name__ == '__main__':
    main()
