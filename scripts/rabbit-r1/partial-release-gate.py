#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate the explicitly scoped partial source snapshot, not the full release."""
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
def main():
    scope = json.loads((ROOT / 'Documentation/rabbit-r1/partial-publication.json').read_text())
    paths = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z']).decode().split('\0')
    excluded = scope['excluded_paths'] + ['drivers/gpu/drm/panel/panel-rabbit-r1-init.h']
    bad = [p for p in paths if any(p == x or p.startswith(x + '/') for x in excluded)]
    if scope['kind'] != 'partial-source-snapshot' or scope['full_device_release'] is not False or bad:
        raise SystemExit('Partial release scope failed: ' + repr(bad))
    for name in ['drivers/gpu/drm/panel/Kconfig', 'drivers/gpu/drm/panel/Makefile']:
        if 'CONFIG_DRM_PANEL_RABBIT_R1' in (ROOT / name).read_text() or 'config DRM_PANEL_RABBIT_R1' in (ROOT / name).read_text():
            raise SystemExit('Omitted panel remains enabled in build files')
    roots = subprocess.check_output(['git', '-C', str(ROOT), 'rev-list', '--max-parents=0', 'HEAD']).decode().split()
    if roots != ['c351d322823fce6d41635cb6e3b582b713e666e3']:
        raise SystemExit('History must descend only from the audited partial root')
    history = subprocess.check_output(['git', '-C', str(ROOT), 'log', '--format=', '--name-only', 'HEAD', '--', *excluded]).strip()
    if history:
        raise SystemExit('Excluded paths occur in publication history')
    if subprocess.check_output(['git', '-C', str(ROOT), 'status', '--porcelain']).strip():
        raise SystemExit('Working tree differs from reviewed commit')
    print('Partial source scope and exclusion-safe history pass; full release remains deferred.')
    return 0
if __name__ == '__main__':
    sys.exit(main())
