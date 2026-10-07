# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("audit_source", Path(__file__).resolve().parents[1] / "audit-source.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class WithheldSources(unittest.TestCase):
    def test_known_paths_rejected_even_with_changed_contents(self):
        for name in audit.WITHHELD_WLAN_PATHS:
            self.assertTrue(audit.withheld_wlan_input(name, b"modified input"))

    def test_renamed_input_rejected_by_content(self):
        data = b"synthetic restricted fixture, no vendor contents"
        with patch.object(audit, "WITHHELD_WLAN_HASHES", {hashlib.sha256(data).hexdigest()}):
            self.assertTrue(audit.withheld_wlan_input("renamed/input.h", data))

    def test_unrelated_kernel_debug_source_allowed(self):
        self.assertFalse(audit.withheld_wlan_input("kernel/debug.c", b"unrelated source"))

    def test_four_precise_hashes_recorded(self):
        self.assertEqual(len(audit.WITHHELD_WLAN_HASHES), 4)
        for value in audit.WITHHELD_WLAN_HASHES:
            self.assertEqual(len(bytes.fromhex(value)), 32)
