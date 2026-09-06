#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
ANALYZER_PATH = PROJECT_ROOT / "scripts" / "derive_roblox_host_abi_profile.py"
# Keep the upgrade fixture independent of the project's current reference.
REFERENCE_PROFILE_PATH = PROJECT_ROOT / "tests" / "fixtures" / "roblox_host_abi_2908.json"
REFERENCE_PAYLOAD_ID = "2908-63c5109637b7d7b2bdb8ed8f858023ff5ef49326"
CANDIDATE_PAYLOAD_ID = "2998-ade08266c67aee88ec9c1d00902150e1684dad3a"
PAYLOAD_ROOT = Path.home() / ".local" / "share" / "mocktail" / "payloads"
REFERENCE_LIBRARY = PAYLOAD_ROOT / REFERENCE_PAYLOAD_ID / "libroblox.so"
CANDIDATE_LIBRARY = PAYLOAD_ROOT / CANDIDATE_PAYLOAD_ID / "libroblox.so"
CANDIDATE_METADATA = PAYLOAD_ROOT / CANDIDATE_PAYLOAD_ID / "roblox_payload.json"
LATEST_REFERENCE_PAYLOAD_ID = "2908-63c5109637b7d7b2bdb8ed8f858023ff5ef49326"
LATEST_CANDIDATE_PAYLOAD_ID = "2998-ade08266c67aee88ec9c1d00902150e1684dad3a"
LATEST_REFERENCE_LIBRARY = PAYLOAD_ROOT / LATEST_REFERENCE_PAYLOAD_ID / "libroblox.so"
LATEST_CANDIDATE_LIBRARY = PAYLOAD_ROOT / LATEST_CANDIDATE_PAYLOAD_ID / "libroblox.so"
LATEST_CANDIDATE_METADATA = (
    PAYLOAD_ROOT / LATEST_CANDIDATE_PAYLOAD_ID / "roblox_payload.json"
)
LATEST_REFERENCE_PROFILE = REFERENCE_PROFILE_PATH


def load_analyzer():
    specification = importlib.util.spec_from_file_location(
        "mocktail_host_abi_analyzer", ANALYZER_PATH
    )
    if specification is None or specification.loader is None:
        raise RuntimeError("cannot load HostAbi analyzer")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


ANALYZER = load_analyzer()


def encode_sleb128(value: int) -> bytes:
    encoded = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        complete = (value == 0 and not byte & 0x40) or (value == -1 and byte & 0x40)
        encoded.append(byte if complete else byte | 0x80)
        if complete:
            return bytes(encoded)


def aps2(*values: int) -> bytes:
    return b"APS2" + b"".join(encode_sleb128(value) for value in values)


def decode_x86(encoded: bytes, address: int = 0x1000):
    decoder = ANALYZER.capstone.Cs(
        ANALYZER.capstone.CS_ARCH_X86, ANALYZER.capstone.CS_MODE_64
    )
    decoder.detail = True
    return tuple(decoder.disasm(encoded, address))


class Aps2DecoderTest(unittest.TestCase):
    def test_decodes_grouped_relative_relocations(self) -> None:
        flags = (
            ANALYZER.APS2_GROUPED_BY_INFO
            | ANALYZER.APS2_GROUPED_BY_OFFSET_DELTA
            | ANALYZER.APS2_GROUPED_BY_ADDEND
            | ANALYZER.APS2_GROUP_HAS_ADDEND
        )
        encoded = aps2(2, 0x1000, 2, flags, 8, 8, 0x2000)

        self.assertEqual(
            list(ANALYZER.decode_aps2_relocations(encoded)),
            [
                ANALYZER.Relocation(0x1008, 8, 0x2000),
                ANALYZER.Relocation(0x1010, 8, 0x2000),
            ],
        )

    def test_rejects_trailing_and_truncated_streams(self) -> None:
        valid_empty = aps2(0, 0)
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "trailing bytes"):
            list(ANALYZER.decode_aps2_relocations(valid_empty + b"\0"))
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "truncated"):
            list(ANALYZER.decode_aps2_relocations(b"APS2\x80"))

    def test_rejects_unknown_group_flags(self) -> None:
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "group is invalid"):
            list(ANALYZER.decode_aps2_relocations(aps2(1, 0, 1, 16)))


class ReferenceProfileTest(unittest.TestCase):
    def setUp(self) -> None:
        self.document = json.loads(REFERENCE_PROFILE_PATH.read_text(encoding="utf-8"))

    def write_document(self, root: Path, document: dict) -> Path:
        destination = root / "reference.json"
        destination.write_text(json.dumps(document), encoding="utf-8")
        return destination

    def test_shipped_reference_is_exact_and_loadable(self) -> None:
        sidecar = ANALYZER.validated_sidecar(REFERENCE_PROFILE_PATH)

        self.assertEqual(sidecar["payload_id"], REFERENCE_PAYLOAD_ID)
        self.assertEqual(sidecar["profile"]["init_array_count"], 3556)
        self.assertEqual(
            sidecar["profile"]["native_pre_jni_bootstrap"]["registry_slot"],
            "0x7a14898",
        )
        self.assertFalse(sidecar.get("status") == "supported")

    def test_rejects_unknown_fields_and_self_reference(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            unknown = copy.deepcopy(self.document)
            unknown["unexpected"] = True
            with self.assertRaisesRegex(ANALYZER.AnalyzerError, "unknown or missing"):
                ANALYZER.validated_sidecar(self.write_document(root, unknown))

            self_reference = copy.deepcopy(self.document)
            self_reference["reference"]["elf_build_id"] = self_reference["elf_build_id"]
            with self.assertRaisesRegex(ANALYZER.AnalyzerError, "provenance identity"):
                ANALYZER.validated_sidecar(self.write_document(root, self_reference))

    def test_rejects_duplicate_allocator_bridge_rva(self) -> None:
        duplicate = copy.deepcopy(self.document)
        duplicate["profile"]["bridge_entries"][1]["rva"] = duplicate["profile"][
            "bridge_entries"
        ][0]["rva"]
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ANALYZER.AnalyzerError, "not unique"):
                ANALYZER.validated_sidecar(
                    self.write_document(Path(temporary), duplicate)
                )


class PayloadMetadataTest(unittest.TestCase):
    def valid_metadata(self, build_id: str, digest: str) -> dict:
        return {
            "schema_version": 1,
            "package": "com.roblox.client",
            "version_name": "2.730.790",
            "version_code": 2718,
            "abi": "x86_64",
            "elf_build_id": build_id,
            "sha256": {"libroblox": digest},
        }

    def test_metadata_must_bind_canonical_payload_directory(self) -> None:
        build_id = "a" * 40
        digest = "b" * 64
        with tempfile.TemporaryDirectory() as temporary:
            payload = Path(temporary) / f"2718-{build_id}"
            payload.mkdir()
            library = payload / "libroblox.so"
            metadata = payload / "roblox_payload.json"
            library.touch()
            metadata.write_text(
                json.dumps(self.valid_metadata(build_id, digest)), encoding="utf-8"
            )
            image = types.SimpleNamespace(build_id=build_id, sha256=digest)

            parsed = ANALYZER.validated_payload_metadata(metadata, library, image)
            self.assertEqual(parsed["version_code"], 2718)

            changed = self.valid_metadata(build_id, "c" * 64)
            metadata.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaisesRegex(ANALYZER.AnalyzerError, "does not match"):
                ANALYZER.validated_payload_metadata(metadata, library, image)


class FailClosedUtilityTest(unittest.TestCase):
    def test_streaming_sha256_works_without_python_311_file_digest(self) -> None:
        payload = b"a" * (ANALYZER.SHA256_CHUNK_BYTES * 2 + 17)

        self.assertEqual(
            ANALYZER.sha256_file(io.BytesIO(payload)),
            hashlib.sha256(payload).hexdigest(),
        )

    def test_streaming_sha256_reports_read_failure(self) -> None:
        class Unreadable:
            def read(self, _size: int) -> bytes:
                raise OSError("synthetic read failure")

        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "cannot hash ELF"):
            ANALYZER.sha256_file(Unreadable())

    def test_missing_capstone_is_an_explicit_failure(self) -> None:
        saved_capstone = ANALYZER.capstone
        saved_x86 = ANALYZER.capstone_x86
        try:
            ANALYZER.capstone = None
            ANALYZER.capstone_x86 = None
            with self.assertRaisesRegex(
                ANALYZER.AnalyzerError, "capstone 5 is required"
            ):
                ANALYZER.require_capstone()
        finally:
            ANALYZER.capstone = saved_capstone
            ANALYZER.capstone_x86 = saved_x86

    def test_unsupported_capstone_major_is_an_explicit_failure(self) -> None:
        saved_capstone = ANALYZER.capstone
        saved_x86 = ANALYZER.capstone_x86
        try:
            ANALYZER.capstone = types.SimpleNamespace(cs_version=lambda: (4, 0, 0))
            ANALYZER.capstone_x86 = types.SimpleNamespace()
            with self.assertRaisesRegex(ANALYZER.AnalyzerError, "major version 5"):
                ANALYZER.require_capstone()
        finally:
            ANALYZER.capstone = saved_capstone
            ANALYZER.capstone_x86 = saved_x86

    def test_constructor_ranges_cannot_overlap(self) -> None:
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "invalid range"):
            ANALYZER.validated_ranges(
                [
                    {"begin": 2, "end_exclusive": 5},
                    {"begin": 4, "end_exclusive": 7},
                ],
                "test ranges",
                8,
            )

    def test_probation_manifest_never_claims_support(self) -> None:
        candidate = types.SimpleNamespace(build_id="a" * 40, sha256="b" * 64)
        metadata = {"version_code": 3000, "version_name": "3.0.0"}
        reference = {"elf_build_id": "c" * 40, "payload_sha256": "d" * 64}

        sidecar, manifest = ANALYZER.output_documents(
            candidate, metadata, reference, {"elf_build_id": "a" * 40}, {"x": 1}
        )

        profile = manifest["profiles"][0]
        self.assertEqual(profile["status"], "experimental")
        self.assertTrue(profile["default_allowed"])
        self.assertFalse(profile["allow_legacy_binary_patches"])
        self.assertEqual(sidecar["payload_path"], f"payloads/{sidecar['payload_id']}")
        self.assertNotIn("supported", profile["status"])

    def test_probation_manifest_preserves_derived_runtime_anchors(self) -> None:
        candidate = types.SimpleNamespace(build_id="a" * 40, sha256="b" * 64)
        metadata = {"version_code": 3000, "version_name": "3.0.0"}
        reference = {"elf_build_id": "c" * 40, "payload_sha256": "d" * 64}
        runtime = {
            "user_game_settings_fullscreen_setter_rva": "0x1234",
            "fmod_output_device_bridge": {"vtable_rva": "0x5678"},
        }

        _sidecar, manifest = ANALYZER.output_documents(
            candidate,
            metadata,
            reference,
            {"elf_build_id": "a" * 40},
            {"x": 1},
            runtime,
        )

        profile = manifest["profiles"][0]
        self.assertEqual(
            profile["user_game_settings_fullscreen_setter_rva"], "0x1234"
        )
        self.assertEqual(
            profile["fmod_output_device_bridge"], {"vtable_rva": "0x5678"}
        )


@unittest.skipIf(ANALYZER.capstone is None, "Python capstone is unavailable")
class SignatureSemanticsTest(unittest.TestCase):
    def test_rip_and_control_flow_targets_may_move(self) -> None:
        reference = decode_x86(bytes.fromhex("488d0534120000e878560000c3"))
        candidate = decode_x86(bytes.fromhex("488d0578560000e834120000c3"), 0x2000)

        ANALYZER.validate_semantic_match(
            reference,
            candidate,
            ANALYZER.SignatureSpec("test", 0x1000, len(reference)),
        )

    def test_registry_object_size_must_change_consistently(self) -> None:
        reference = decode_x86(
            b"\x90" * 8
            + bytes.fromhex("bf38030000")
            + b"\x90" * 3
            + bytes.fromhex("be38030000")
        )
        candidate = decode_x86(
            b"\x90" * 8
            + bytes.fromhex("bfb0030000")
            + b"\x90" * 3
            + bytes.fromhex("beb0030000")
        )
        inconsistent = decode_x86(
            b"\x90" * 8
            + bytes.fromhex("bfb0030000")
            + b"\x90" * 3
            + bytes.fromhex("beb8030000")
        )

        spec = ANALYZER.SignatureSpec(
            "registry", 0x1000, len(reference), True
        )
        ANALYZER.validate_semantic_match(reference, candidate, spec)
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "inconsistent"):
            ANALYZER.validate_semantic_match(reference, inconsistent, spec)

    def test_registry_body_can_use_shifted_size_indices(self) -> None:
        reference = decode_x86(
            bytes.fromhex(
                "bf38030000 e800000000 4889c3 4531f6 ba38030000 "
                "4889c7 31f6 e800000000 b80000803f 894320 488d4b28"
            )
        )
        candidate = decode_x86(
            bytes.fromhex(
                "bfb0030000 e800000000 4889c3 4531f6 bab0030000 "
                "4889c7 31f6 e800000000 b80000803f 894320 488d4b28"
            ),
            0x2000,
        )
        spec = ANALYZER.SignatureSpec(
            "registry-body", 0x1000, len(reference), True, 6, (0, 4)
        )

        ANALYZER.validate_semantic_match(reference, candidate, spec)

    def test_short_anchor_is_opt_in(self) -> None:
        encoded = b"abc\0def"
        mask = b"\x01\x01\x01\0\x01\x01\x01"
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "no selective"):
            ANALYZER.longest_fixed_anchor(encoded, mask)
        self.assertEqual(ANALYZER.longest_fixed_anchor(encoded, mask, 3), (0, b"abc"))


@unittest.skipIf(ANALYZER.capstone is None, "Python capstone is unavailable")
class RegistrySlotDerivationTest(unittest.TestCase):
    class Image:
        def __init__(self, encoded: bytes, writable_slot: int):
            self.data = encoded
            self.file_size = len(encoded)
            self.writable_slot = writable_slot

        def require_code_rva(self, rva: int, size: int = 1) -> None:
            if rva != 0x1000 or size != 1:
                raise ANALYZER.AnalyzerError("unexpected code RVA")

        def rva_to_offset(self, rva: int, size: int = 1) -> int:
            if rva != 0x1000 or size != 1:
                raise ANALYZER.AnalyzerError("unexpected RVA mapping")
            return 0

        def bytes_at(self, offset: int, size: int, _description: str) -> bytes:
            if offset != 0 or size < 0 or size > len(self.data):
                raise ANALYZER.AnalyzerError("unexpected byte range")
            return self.data[:size]

        def require_writable_rva(self, rva: int, size: int = 1) -> None:
            if rva != self.writable_slot or size != 8:
                raise ANALYZER.AnalyzerError("registry slot is not writable")

    @staticmethod
    def store_rbx(instruction_rva: int, target_rva: int) -> bytes:
        displacement = target_rva - (instruction_rva + 7)
        return b"\x48\x89\x1d" + struct.pack("<i", displacement)

    def test_derives_only_unique_aligned_store_before_first_return(self) -> None:
        encoded = self.store_rbx(0x1000, 0x2000) + b"\x90\xc3"
        image = self.Image(encoded, 0x2000)

        self.assertEqual(ANALYZER.derive_registry_slot(image, 0x1000), 0x2000)

    def test_rejects_missing_return_and_duplicate_slot_stores(self) -> None:
        without_return = self.Image(self.store_rbx(0x1000, 0x2000) + b"\x90", 0x2000)
        duplicate_stores = self.Image(
            self.store_rbx(0x1000, 0x2000) + self.store_rbx(0x1007, 0x3000) + b"\xc3",
            0x2000,
        )

        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "bounded return"):
            ANALYZER.derive_registry_slot(without_return, 0x1000)
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "one unique slot"):
            ANALYZER.derive_registry_slot(duplicate_stores, 0x1000)

    def test_rejects_unaligned_slot_and_ignores_code_after_return(self) -> None:
        unaligned = self.Image(self.store_rbx(0x1000, 0x2001) + b"\xc3", 0x2001)
        store_after_return = self.Image(
            b"\xc3" + self.store_rbx(0x1001, 0x2000), 0x2000
        )

        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "pointer-aligned"):
            ANALYZER.derive_registry_slot(unaligned, 0x1000)
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "one unique slot"):
            ANALYZER.derive_registry_slot(store_after_return, 0x1000)

    def test_reference_sidecar_slot_must_match_its_initializer(self) -> None:
        image = self.Image(self.store_rbx(0x1000, 0x2000) + b"\xc3", 0x2000)
        bootstrap = {
            "registry_initializer": "0x1000",
            "registry_slot": "0x2000",
        }

        ANALYZER.validate_reference_registry_slot(image, bootstrap)
        bootstrap["registry_slot"] = "0x2008"
        with self.assertRaisesRegex(ANALYZER.AnalyzerError, "does not match"):
            ANALYZER.validate_reference_registry_slot(image, bootstrap)


@unittest.skipUnless(
    ANALYZER.capstone is not None
    and REFERENCE_LIBRARY.is_file()
    and CANDIDATE_LIBRARY.is_file()
    and CANDIDATE_METADATA.is_file(),
    "local exact 2908 and 2998 payloads are unavailable",
)
class RealPayloadAcceptanceTest(unittest.TestCase):
    def test_exact_2908_to_2998_derivation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "profile.json"
            compatibility = Path(temporary) / "compatibility.json"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ANALYZER_PATH),
                    "--reference-lib",
                    str(REFERENCE_LIBRARY),
                    "--reference-profile",
                    str(REFERENCE_PROFILE_PATH),
                    "--reference-compatibility",
                    str(PROJECT_ROOT / "config" / "roblox_compatibility.json"),
                    "--candidate-lib",
                    str(CANDIDATE_LIBRARY),
                    "--payload-metadata",
                    str(CANDIDATE_METADATA),
                    "--output",
                    str(output),
                    "--compatibility-output",
                    str(compatibility),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            sidecar = json.loads(output.read_text(encoding="utf-8"))
            manifest = json.loads(compatibility.read_text(encoding="utf-8"))

            self.assertEqual(sidecar["payload_id"], CANDIDATE_PAYLOAD_ID)
            self.assertEqual(
                sidecar["payload_sha256"],
                "4343a6a900d1fca27ff8e10b9d9c86bee40a5d0ba547cb29903b7769f4f42a9d",
            )
            self.assertEqual(
                sidecar["profile"]["native_allocator"],
                {"allocate": "0x1d2bf82", "deallocate": "0x1d2f9d9"},
            )
            self.assertEqual(sidecar["profile"]["init_array_count"], 3570)
            self.assertEqual(
                sidecar["profile"]["native_pre_jni_bootstrap"],
                {
                    "registry_initializer": "0x21e8e09",
                    "registry_slot": "0x7af4598",
                },
            )
            self.assertEqual(manifest["profiles"][0]["status"], "experimental")
            self.assertEqual(
                manifest["profiles"][0]["user_game_settings_fullscreen_setter_rva"],
                "0x45ad8aa",
            )
            self.assertEqual(
                manifest["profiles"][0]["fmod_output_device_bridge"]["vtable_rva"],
                "0x6c58040",
            )


@unittest.skipUnless(
    ANALYZER.capstone is not None
    and LATEST_REFERENCE_LIBRARY.is_file()
    and LATEST_REFERENCE_PROFILE.is_file()
    and LATEST_CANDIDATE_LIBRARY.is_file()
    and LATEST_CANDIDATE_METADATA.is_file(),
    "local exact 2908 and 2998 payloads are unavailable",
)
class RealRuntimeCompatibilityAcceptanceTest(unittest.TestCase):
    def test_exact_2908_to_2998_runtime_bridge_derivation(self) -> None:
        with ANALYZER.ElfImage(LATEST_REFERENCE_LIBRARY) as reference, ANALYZER.ElfImage(
            LATEST_CANDIDATE_LIBRARY
        ) as candidate:
            derived = ANALYZER.derive_runtime_compatibility(
                reference,
                candidate,
                (PROJECT_ROOT / "config" / "roblox_compatibility.json",),
            )

        self.assertEqual(
            derived,
            {
                "user_game_settings_fullscreen_setter_rva": "0x45ad8aa",
                "fmod_output_device_bridge": {
                    "vtable_rva": "0x6c58040",
                    "string_constructor_rva": "0x1d32df8",
                    "count_method_rva": "0x32d0ee0",
                    "info_method_rva": "0x32d0f80",
                    "current_method_rva": "0x32d0f30",
                    "select_method_rva": "0x32d0cb4",
                },
            },
        )


if __name__ == "__main__":
    unittest.main()
