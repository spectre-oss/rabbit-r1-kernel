#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare optional sources offline in a new directory; never install or flash."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

BUNDLE = Path(__file__).resolve().parent
ROOT = BUNDLE.parents[1]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def checked_file(root, name, expected):
    path = root / name
    if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(root):
        raise ValueError('Missing or unsafe input: ' + name)
    if digest(path) != expected:
        raise ValueError('Input hash mismatch: ' + name)
    return path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rabbit-kernel', required=True, type=Path)
    parser.add_argument('--rabbit-modules', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    kernel, modules = args.rabbit_kernel.resolve(), args.rabbit_modules.resolve()
    output = args.output.resolve()
    if not args.output.is_absolute() or args.output.is_symlink() or output.exists():
        parser.error('Output must be a new absolute directory')
    if any(output.is_relative_to(p) for p in (ROOT, kernel, modules)):
        parser.error('Output must be outside the publication and vendor source trees')
    manifest = json.loads((BUNDLE / 'manifest.json').read_text())
    try:
        checked_file(kernel, manifest['panel_source'], manifest['panel_sha256'])
        for row in manifest['vendor_files']:
            checked_file(modules, row['source'], row['source_sha256'])
        for name, expected in manifest['patch_sha256'].items():
            checked_file(BUNDLE, name, expected)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    subprocess.run(['git', '-C', str(ROOT), 'cat-file', '-e', manifest['spectre_base_commit'] + '^{commit}'], check=True)
    output.mkdir(parents=True, mode=0o700)
    archive = subprocess.Popen(['git', '-C', str(ROOT), 'archive', manifest['spectre_base_commit']], stdout=subprocess.PIPE)
    try:
        subprocess.run(['tar', '-x', '-C', str(output)], stdin=archive.stdout, check=True)
    finally:
        archive.stdout.close()
    if archive.wait() != 0:
        raise RuntimeError('Base archive failed; output is incomplete')
    with tempfile.TemporaryDirectory(prefix='rabbit-patch-') as temporary:
        stage = Path(temporary)
        for row in manifest['vendor_files']:
            target = stage / row['source']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(modules / row['source'], target)
        patch = BUNDLE / '0001-rabbit-wlan-linux71.patch'
        subprocess.run(['git', 'apply', '--check', str(patch)], cwd=stage, check=True)
        subprocess.run(['git', 'apply', str(patch)], cwd=stage, check=True)
        for row in manifest['vendor_files']:
            source = checked_file(stage, row['source'], row['prepared_sha256'])
            target = output / row['destination']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    for row in manifest['integration_files']:
        target = output / row['path']
        if row['before_sha256'] is None:
            if target.exists():
                raise ValueError('Unexpected existing integration file: ' + row['path'])
        else:
            checked_file(output, row['path'], row['before_sha256'])
    patch = BUNDLE / '0002-spectre-optional-integration.patch'
    subprocess.run(['git', 'apply', '--check', str(patch)], cwd=output, check=True)
    subprocess.run(['git', 'apply', str(patch)], cwd=output, check=True)
    for row in manifest['integration_files']:
        checked_file(output, row['path'], row['after_sha256'])
    # New script created by a plain patch does not inherit executable mode.
    (output / 'scripts/rabbit-r1/prepare-panel.py').chmod(0o755)
    print('Verified 220 vendor inputs and 11 integration files in:', output)
    print('Panel table and four deferred WLAN inputs remain external. See the patch README for build commands.')

if __name__ == '__main__':
    main()
