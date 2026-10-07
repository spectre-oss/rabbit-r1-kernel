#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract hash-pinned stock GPU files offline. Never mounts, executes or installs blobs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

BUNDLE = Path(__file__).resolve().parent
ROOT = BUNDLE.parents[1]
MAX_FILE_BYTES = 64 * 1024 * 1024

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def validate_output(path):
    if not path.is_absolute() or path.is_symlink() or path.exists():
        raise ValueError('Output must be a new absolute directory')
    output = path.resolve()
    if output.is_relative_to(ROOT) or any((p / '.git').exists() for p in output.parents):
        raise ValueError('Keep proprietary outputs outside all Git checkouts')
    if not output.parent.is_dir():
        raise ValueError('Output parent must already exist')
    return output

def extract(images, output, manifest, runner=subprocess.run):
    output = validate_output(output)
    rows = [r for r in manifest['files'] if r['partition'] in images]
    seen = set()
    for row in rows:
        source = row['source']
        if not re.fullmatch(r'/[A-Za-z0-9_./@+\-]+', source) or '..' in Path(source).parts:
            raise ValueError('Unsafe source path in manifest')
        key = (row['partition'], source)
        if key in seen or not re.fullmatch(r'[0-9a-f]{64}', row['sha256']):
            raise ValueError('Invalid manifest entry')
        seen.add(key)
    for path in images.values():
        if path.is_symlink() or not path.is_file():
            raise ValueError('Supply a regular raw ext4 partition image')
    output.mkdir(mode=0o700)
    results = []
    try:
        with tempfile.TemporaryDirectory(prefix='r1-gpu-extract-') as scratch:
            scratch = Path(scratch)
            for row in rows:
                payload = scratch / 'payload'
                payload.unlink(missing_ok=True)
                # Fixed destination and validated manifest paths; no shell or debugfs write mode.
                runner(['debugfs', '-R', 'dump ' + row['source'] + ' payload', str(images[row['partition']].resolve())],
                       cwd=scratch, check=True, capture_output=True, timeout=60)
                if not payload.is_file() or payload.is_symlink() or payload.stat().st_size > MAX_FILE_BYTES:
                    raise ValueError('Missing or oversized blob: ' + row['source'])
                if digest(payload) != row['sha256']:
                    raise ValueError('Blob hash mismatch: ' + row['source'])
                target = output / row['partition'] / row['source'].lstrip('/')
                target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                shutil.copyfile(payload, target)
                target.chmod(0o600)
                results.append({'path': str(target.relative_to(output)), 'sha256': row['sha256']})
        (output / 'extraction.json').write_text(json.dumps({'reference': manifest['reference'], 'files': results,
            'installed': False, 'limits': 'Stock files only; not a configured runnable GPU stack. Do not commit these blobs.'}, indent=2) + '\n')
        (output / 'extraction.json').chmod(0o600)
    except BaseException:
        # Only remove the new output directory exclusively created by this invocation.
        shutil.rmtree(output)
        raise
    return results

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vendor-image', required=True, type=Path)
    parser.add_argument('--system-image', type=Path, help='Optional matching system_a.img for Android runtime dependencies')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if shutil.which('debugfs') is None:
        parser.error('debugfs is required (e2fsprogs); no tools are installed automatically')
    os.umask(0o077)
    images = {'vendor': args.vendor_image}
    if args.system_image:
        images['system'] = args.system_image
    try:
        results = extract(images, args.output, json.loads((BUNDLE / 'gpu-blobs.json').read_text()))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, 'Extraction failed: ' + str(error) + '\n')
    print(f'Verified {len(results)} files in {args.output}. Nothing installed or executed.')
    if not args.system_image:
        print('System/Bionic runtime dependencies were not extracted; this is not a runnable graphics stack.')

if __name__ == '__main__':
    main()
