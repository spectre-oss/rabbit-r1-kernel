#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Scan the proposed source payload; report paths, never matched secret values.

This is a bounded publication guard, not a certification that all secrets or
license issues have been detected. --private-terms-file accepts local-only
identifiers to exclude without committing those identifiers into this project.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
# Public upstream fixture, verified through the pinned upstream Git tree.
FIXTURES = {
    'tools/testing/selftests/sgx/sign_key.pem':
    '1f6566d02247dc09d4dcb542ee2399522ceebf9b435ee00fe2053aa5582541e2',
}
PATTERNS = {
    'private-key-block': re.compile(rb'-----BEGIN (?:OPENSSH |RSA |EC |DSA |ENCRYPTED )?PRIVATE KEY-----[\s\S]{30,}?-----END (?:OPENSSH |RSA |EC |DSA |ENCRYPTED )?PRIVATE KEY-----'),
    'github-token': re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{36,255}|github_pat_[A-Za-z0-9_]{60,255})\b'),
    'provider-key': re.compile(rb'\bsk-(?:proj-|or-v1-)[A-Za-z0-9_-]{32,}\b'),
}

# Exact withheld vendor inputs; see Documentation/rabbit-r1/redistribution-review.md.
WITHHELD_WLAN_PATHS = {
    'external/rabbit-r1/wlan/mgmt/tkip_mic.c',
    'external/rabbit-r1/wlan/common/debug.c',
    'external/rabbit-r1/wlan/include/wsys_cmd_handler_fw.h',
    'external/rabbit-r1/wlan/mgmt/reg_rule.c',
}
WITHHELD_WLAN_HASHES = {
    '3388c446e995e4c58abdb499c25659b76d74282b07f73d92e489c57684c7f7db',
    '684213f7a6dc783a30397b1e8b91ec1c996388699c1fec6ffdb8ec3e3924aca1',
    'd75ac0716b9cf8c04af26ab53864e968d7dce44e7a1f798b20431b93b9a3c880',
    'b10cb95494aa292ce6db6580f0b68a50de83bd477f2b6f95d6d7f7474f7229e3',
}

def withheld_wlan_input(name, data):
    return name in WITHHELD_WLAN_PATHS or hashlib.sha256(data).hexdigest() in WITHHELD_WLAN_HASHES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-terms-file', type=Path)
    args = parser.parse_args()
    terms = []
    if args.private_terms_file:
        terms = [line.strip() for line in args.private_terms_file.read_bytes().splitlines() if line.strip()]
    raw = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '--cached', '--others', '--exclude-standard', '-z'])
    findings = []
    scanned = 0
    for name in sorted(set(raw.decode().split('\0')) - {''}):
        if any(term in name.encode() for term in terms):
            findings.append({'path': '[redacted private filename]', 'finding': 'private-term-in-path'})
            continue
        path = ROOT / name
        if path.is_symlink():
            if str(path.readlink()).startswith('/') or not path.resolve().is_relative_to(ROOT):
                findings.append({'path': name, 'finding': 'external-symlink'})
            continue
        if not path.is_file():
            continue
        data = path.read_bytes()
        scanned += 1
        if withheld_wlan_input(name, data):
            findings.append({'path': name, 'finding': 'unresolved-wlan-license'})
        if name in FIXTURES and hashlib.sha256(data).hexdigest() == FIXTURES[name]:
            continue
        if data.startswith((b'\x7fELF', b'SQLite format 3\0')):
            findings.append({'path': name, 'finding': 'binary-or-database'})
        for label, pattern in PATTERNS.items():
            if pattern.search(data):
                findings.append({'path': name, 'finding': label})
        if any(term in data for term in terms):
            findings.append({'path': name, 'finding': 'private-term'})
        if name.endswith(('rabbit-r1-panel-init.h', 'panel-rabbit-r1-init.h', 'rabbit-r1-camera-receiver.h', 'receiver-steps.h', 'rabbit-r1-encoder-modes.h', 'r1_venc_modes.h', 'private-encoder-modes.json')):
            findings.append({'path': name, 'finding': 'private-generated-hardware-input'})
    print(json.dumps({'scanned_files': scanned, 'findings': findings}, indent=2))
    return 1 if findings else 0


if __name__ == '__main__':
    raise SystemExit(main())
