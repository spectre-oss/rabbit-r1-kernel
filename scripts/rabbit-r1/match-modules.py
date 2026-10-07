#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare private device inventory with local artifact hashes; never load modules.

An exact file hash proves artifact identity, not its corresponding source or license.
Loaded build notes corroborate identity but do not replace a full file hash.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path


def match(inventory, artifacts):
    by_hash = defaultdict(list)
    installed_by_name = defaultdict(list)
    for row in artifacts:
        by_hash[row['sha256']].append(row['path'])
    installed = []
    for row in inventory.get('installed_module_files', []):
        candidates = sorted(set(by_hash.get(row['sha256'], [])))
        installed.append({'path': row['path'], 'name': row.get('name'),
                          'sha256': row['sha256'], 'local_artifacts': candidates,
                          'status': 'exact_artifact_match' if candidates else 'unmatched'})
        if row.get('name'):
            installed_by_name[row['name']].append(row)
    loaded = []
    for row in inventory.get('loaded_modules', []):
        note = row.get('build_note_sha256')
        candidates = [candidate for candidate in installed_by_name[row['name']]
                      if note and candidate.get('build_note_sha256') == note]
        hashes = {candidate['sha256'] for candidate in candidates}
        status = ('missing_build_note' if not note else
                  'no_installed_match' if not candidates else
                  'ambiguous_artifacts' if len(hashes) > 1 else
                  'matched_installed_artifact' if next(iter(hashes)) in by_hash else
                  'installed_artifact_not_indexed')
        loaded.append({'name': row['name'], 'status': status,
                       'installed_candidates': sorted(candidate['path'] for candidate in candidates)})
    errors = inventory.get('errors', [])
    complete = (bool(installed) and bool(loaded) and not errors and
                all(row['status'] == 'exact_artifact_match' for row in installed) and
                all(row['status'] == 'matched_installed_artifact' for row in loaded))
    return {'schema_version': 1, 'artifact_correspondence_complete': complete,
            'source_correspondence_verified': False,
            'installed': installed, 'loaded': loaded, 'inventory_errors': errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventory', type=Path)
    parser.add_argument('artifact_index', type=Path)
    args = parser.parse_args()
    report = match(json.loads(args.inventory.read_text()), json.loads(args.artifact_index.read_text()))
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report['artifact_correspondence_complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
