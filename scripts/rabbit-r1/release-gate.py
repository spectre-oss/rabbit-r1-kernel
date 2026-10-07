#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fail closed while the release's required evidence is incomplete."""
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
status = json.loads((root / 'Documentation/rabbit-r1/release-status.json').read_text())
required = {
    'upstream_reference_git_objects', 'live_device_identity',
    'kernel_source_correspondence', 'external_module_correspondence',
    'embedded_init_corresponding_source', 'panel_table_redistribution',
    'full_license_review', 'privacy_review', 'clean_release_build',
    'wlan_input_redistribution', 'camera_receiver_input_provenance',
    'encoder_mode_input_redistribution',
}
checks = status.get('checks', {})
missing = sorted(key for key in required if checks.get(key) != 'pass')
if status.get('schema_version') != 1 or status.get('publication_ready') is not True or missing:
    print('Release blocked: evidence is incomplete.', file=sys.stderr)
    for name in missing:
        print(f'  {name}: {checks.get(name, "missing")}', file=sys.stderr)
    sys.exit(1)
print('Recorded release gates pass. Review the evidence before publishing.')
