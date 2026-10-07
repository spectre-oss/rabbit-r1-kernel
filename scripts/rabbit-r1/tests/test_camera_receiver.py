# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('receiver', Path(__file__).resolve().parents[1] / 'prepare-camera-receiver.py')
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)


def table(body):
    return 'struct receiver_step {u32 physical,keep,value;};\nstatic const struct receiver_step receiver_steps[] = {\n' + body + '\n};\n'


class ReceiverTests(unittest.TestCase):
    def test_numeric_fixture_round_trip(self):
        source = table('{0x1a040000,0xffffffff,0}, {0x11c10000,0,1},')
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.h'; path.write_text(source)
            output = Path(temp) / 'build'
            self.assertEqual(receiver.prepare(path, output, hashlib.sha256(path.read_bytes()).hexdigest()), 2)
            generated = output / 'include/generated/rabbit-r1-camera-receiver.h'
            self.assertEqual(receiver.extract(generated.read_text()), receiver.extract(source))

    def test_rejects_code_and_expressions(self):
        for body in ('{0x1a040000,0,run_code()},', '{0x1a040000,0,1<<4},', '{0x1a040000,0,1}, arbitrary_code;'):
            with self.assertRaises(ValueError): receiver.extract(table(body))
        with self.assertRaises(ValueError): receiver.extract(table('{0x1a040000,0,1},') + 'void execute(void) {}')

    def test_rejects_unmapped_or_unaligned_register(self):
        for address in ('0x1a048000', '0x11c16000', '0x1a040001', '0'):
            with self.assertRaises(ValueError): receiver.extract(table('{' + address + ',0,0},'))

    def test_rejects_oversized_values_and_table(self):
        with self.assertRaises(ValueError): receiver.extract(table('{0x1a040000,0,0x100000000},'))
        with self.assertRaises(ValueError): receiver.extract(table('{0x1a040000,0,0},' * 513))
        with self.assertRaises(ValueError): receiver.extract(table(''))

    def test_hash_mismatch_creates_no_output(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'input.h'; source.write_text(table('{0x1a040000,0,0},'))
            output = Path(temp) / 'build'
            with self.assertRaises(ValueError): receiver.prepare(source, output, '0' * 64)
            self.assertFalse(output.exists())

    def test_source_tree_alias_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            alias = Path(temp) / 'alias'; alias.symlink_to(receiver.ROOT, target_is_directory=True)
            with self.assertRaises(ValueError): receiver.prepare(Path(temp) / 'missing', alias / 'must-not-create', '0' * 64)
            self.assertFalse((alias / 'must-not-create').exists())


if __name__ == '__main__':
    unittest.main()
