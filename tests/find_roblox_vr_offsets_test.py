"""Regression coverage for the changed world-pointer ABI in Roblox 3222."""

import importlib.util
import json
from pathlib import Path
import struct
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "mocktail_vr_offsets", ROOT / "scripts/find_roblox_vr_offsets.py")
offsets = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = offsets
SPEC.loader.exec_module(offsets)


@unittest.skipIf(offsets.capstone is None, "Python Capstone is not installed")
class VrPointerAnalysisTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        offsets.require_capstone()
        cls.fixture = json.loads(
            (ROOT / "tests/vr/fixtures/roblox_3222_contracts.json").read_text())
        function = cls.fixture["functions"]["pointer"]
        cls.code = bytes.fromhex(function["bytes"])
        cls.rva = int(function["rva"], 16)
        cls.decoder = offsets.capstone.Cs(
            offsets.capstone.CS_ARCH_X86, offsets.capstone.CS_MODE_64)
        cls.decoder.detail = True

    def decode(self, code):
        return tuple(self.decoder.disasm(code, self.rva))

    def test_recognizes_recorded_3222_layout_and_workspace_path(self):
        instructions = self.decode(self.code)
        self.assertTrue(offsets.match_pointer_getter(
            instructions, 0x108, 0x16C, 0x130))
        self.assertFalse(offsets.match_pointer_getter(instructions))

        class RecordedImage:
            def require_code_rva(self, rva):
                if rva != 0x264BB76:
                    raise offsets.AnalyzerError("unexpected workspace target")

        self.assertEqual(offsets.extract_pointer_getter_anchors(
            instructions, RecordedImage()), (0x264BB76, 0x3A0))

    def test_keeps_the_older_layout_recognizable(self):
        code = bytearray(self.code)
        for position, value in ((0x27, 0x118), (0x37, 0x174),
                                (0x3F, 0x184), (0x47, 0x194), (0x142, 0x140)):
            struct.pack_into("<I", code, position, value)
        instructions = self.decode(code)
        self.assertTrue(offsets.match_pointer_getter(instructions))
        self.assertFalse(offsets.match_pointer_getter(
            instructions, 0x108, 0x16C, 0x130))

    def test_rejects_mixed_or_unknown_field_offsets(self):
        for position in (0x27, 0x37, 0x3F, 0x47, 0x142):
            with self.subTest(position=position):
                code = bytearray(self.code)
                code[position] ^= 1
                instructions = self.decode(code)
                self.assertFalse(any(offsets.match_pointer_getter(
                    instructions, *layout) for layout in offsets.POINTER_LAYOUTS))

    def test_rejects_a_missing_workspace_handoff(self):
        code = bytearray(self.code)
        code[0x56:0x59] = b"\x90\x90\x90"
        self.assertFalse(offsets.match_pointer_getter(
            self.decode(code), 0x108, 0x16C, 0x130))


if __name__ == "__main__":
    unittest.main()
