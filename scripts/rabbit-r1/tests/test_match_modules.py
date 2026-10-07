# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('match_modules', Path(__file__).resolve().parents[1] / 'match-modules.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CorrespondenceTests(unittest.TestCase):
    def inventory(self):
        return {'installed_module_files': [{'name': 'example', 'path': '/usr/lib/r1-test/example.ko',
                'sha256': 'file-hash', 'build_note_sha256': 'note-hash'}],
                'loaded_modules': [{'name': 'example', 'build_note_sha256': 'note-hash'}], 'errors': []}

    def report(self, inventory):
        return module.match(inventory, [{'path': 'build/example.ko', 'sha256': 'file-hash'}])

    def test_exact_artifact_does_not_claim_source_proof(self):
        result = self.report(self.inventory())
        self.assertTrue(result['artifact_correspondence_complete'])
        self.assertFalse(result['source_correspondence_verified'])

    def test_same_name_different_loaded_binary_is_not_a_match(self):
        inventory = self.inventory()
        inventory['loaded_modules'][0]['build_note_sha256'] = 'different'
        self.assertFalse(self.report(inventory)['artifact_correspondence_complete'])

    def test_missing_notes_do_not_match_each_other(self):
        inventory = self.inventory()
        for key in ('installed_module_files', 'loaded_modules'):
            inventory[key][0].pop('build_note_sha256')
        self.assertEqual(self.report(inventory)['loaded'][0]['status'], 'missing_build_note')

    def test_same_note_different_files_is_ambiguous(self):
        inventory = self.inventory()
        other = dict(inventory['installed_module_files'][0], path='/other/example.ko', sha256='other-hash')
        inventory['installed_module_files'].append(other)
        self.assertEqual(self.report(inventory)['loaded'][0]['status'], 'ambiguous_artifacts')

    def test_unindexed_installed_file_does_not_pass(self):
        inventory = self.inventory()
        inventory['installed_module_files'][0]['sha256'] = 'unknown'
        self.assertFalse(self.report(inventory)['artifact_correspondence_complete'])

    def test_partial_inventory_error_does_not_pass(self):
        inventory = self.inventory()
        inventory['errors'] = [{'path': '/unreadable.ko', 'error_type': 'OSError'}]
        self.assertFalse(self.report(inventory)['artifact_correspondence_complete'])

    def test_empty_inventory_does_not_pass(self):
        self.assertFalse(self.report({})['artifact_correspondence_complete'])


if __name__ == '__main__':
    unittest.main()
