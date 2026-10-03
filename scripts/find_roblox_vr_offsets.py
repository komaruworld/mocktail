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
"""Find native VR bridge offsets in an x86-64 Roblox ELF.

Usage:
    python3 find_roblox_vr_offsets.py --elf libroblox.so --output offsets.json
"""

from __future__ import annotations

import argparse
import bisect
from collections.abc import Iterator, Sequence
import dataclasses
import hashlib
import json
import mmap
import os
import pathlib
import re
import stat
import struct
import sys
import tempfile
from typing import Any

try:
    import capstone
    from capstone import x86 as capstone_x86
except ImportError:
    capstone = None
    capstone_x86 = None

MAX_ELF_BYTES = 512 * 1024 * 1024
MAX_RELA_DYN_BYTES = 256 * 1024 * 1024
MAX_RELOCATIONS = 4 * 1024 * 1024
MAX_FUNCTION_TABLE_ENTRIES = 2 * 1024 * 1024
MAX_FUNCTION_BYTES = 0x20000
MAX_JSON_OUTPUT_BYTES = 4 * 1024 * 1024
SHA256_CHUNK_BYTES = 1024 * 1024

ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
NOTE_HEADER = struct.Struct("<III")

ELF_CLASS_64 = 2
ELF_DATA_LITTLE_ENDIAN = 1
ELF_VERSION_CURRENT = 1
ELF_TYPE_SHARED_OBJECT = 3
ELF_MACHINE_X86_64 = 62
PT_LOAD = 1
PT_GNU_RELRO = 0x6474E552
PF_EXECUTE = 1
PF_WRITE = 2
PF_READ = 4
SHT_PROGBITS = 1
SHT_NOTE = 7
SHT_ANDROID_RELA = 0x60000002
SHT_NOBITS = 8
NT_GNU_BUILD_ID = 3

APS2_GROUPED_BY_INFO = 1
APS2_GROUPED_BY_OFFSET_DELTA = 2
APS2_GROUPED_BY_ADDEND = 4
APS2_GROUP_HAS_ADDEND = 8

R_X86_64_RELATIVE = 8
R_X86_64_IRELATIVE = 1027

# Guest ABI layout: src/vr/roblox_vr_device_bridge_internal.h.
RTTI_TYPE_NAME = b"N3RBX8Graphics13DebugDeviceVRE"
DEVICE_OBJECT_NAME = b"DebugDevice"
EYE_RESOURCE_LABEL = b"VR Eye"
EMULATOR_FLAG_NAME = b"DebugEnableVREmulator"
HAND_PITCH_FLAG_NAME = b"NewVRSystemHandPitch"

OBJECT_TYPE_OFFSET = 0x10
OBJECT_TYPE_DEBUG_DEVICE = 6
STATE_OFFSET = 0x14
STATE_COPY_SIZE = 0x138
CONSTRUCTOR_MEMSET_SIZE = 0x134
READY_BYTE_OFFSETS = (0x94, 0x144, 0x147)
FRAMEBUFFER_SLOT_OFFSET = 0x150
TEXTURE_SLOT_OFFSET = 0x170
USERCFRAME_SELECTOR_OFFSET = 0x118
USERCFRAME_BASE_OFFSET = 0x174
USERCFRAME_STRIDE = 0x30
CAMERA_HEAD_SCALE_OFFSET = 0x140

EXPECTED_STATE_GETTER_SLOT = 0x28
EXPECTED_EYE_GETTER_SLOT = 0x48
EXPECTED_EYE_INITIALIZER_SLOT = 0xD0
EXPECTED_HAPTIC_SLOT = 0x18
MAX_VTABLE_SCAN_BYTES = 0x200

# movsxd rax, [rsi+0x118]; movups xmm0, [rsi+rax+0x174].
POINTER_GETTER_SELECTOR_PATTERN = bytes.fromhex("48 63 86 18 01 00 00")
POINTER_GETTER_MOVUPS_PATTERN = bytes.fromhex("0f 10 84 06 74 01 00 00")

TEXT_REF_OPCODES = re.compile(
    rb"[\x8d\x8b\x89\x8a\x88\x3b\x39\x3a\x38\x03\x33\x2b\x85\x80\x81\x83"
    rb"\xc7\xc6\xff]")
TEXT_REF_MODRMS = re.compile(rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]")


class AnalyzerError(RuntimeError):
    """Raised when VR offset analysis rejects an input."""


@dataclasses.dataclass(frozen=True)
class Segment:
    offset: int
    virtual_address: int
    file_size: int
    memory_size: int
    flags: int

    def contains_memory(self, rva: int, size: int = 1) -> bool:
        return (size >= 0 and self.virtual_address <= rva and
                rva + size <= self.virtual_address + self.memory_size)

    def contains_file_data(self, rva: int, size: int = 1) -> bool:
        return (size >= 0 and self.virtual_address <= rva and
                rva + size <= self.virtual_address + self.file_size)


@dataclasses.dataclass(frozen=True)
class Section:
    name: str
    section_type: int
    flags: int
    address: int
    offset: int
    size: int


@dataclasses.dataclass(frozen=True)
class Relocation:
    offset: int
    info: int
    addend: int


def sha256_file(file_object: Any) -> str:
    digest = hashlib.sha256()
    while True:
        chunk = file_object.read(SHA256_CHUNK_BYTES)
        if not chunk:
            return digest.hexdigest()
        digest.update(chunk)


class Sleb128Reader:

    def __init__(self, encoded: bytes):
        self._encoded = encoded
        self.position = 0

    def pop(self) -> int:
        value = 0
        shift = 0
        while True:
            if self.position >= len(self._encoded) or shift >= 70:
                raise AnalyzerError("invalid or truncated APS2 SLEB128 value")
            byte = self._encoded[self.position]
            self.position += 1
            value |= (byte & 0x7F) << shift
            shift += 7
            if (byte & 0x80) == 0:
                break
        if shift < 64 and (byte & 0x40):
            value -= 1 << shift
        if not -(1 << 63) <= value < (1 << 64):
            raise AnalyzerError("APS2 SLEB128 value exceeds 64-bit bounds")
        return value

    def at_end(self) -> bool:
        return self.position == len(self._encoded)


def decode_aps2_relocations(encoded: bytes) -> Iterator[Relocation]:
    if not encoded.startswith(b"APS2"):
        raise AnalyzerError(".rela.dyn does not use the required APS2 encoding")
    reader = Sleb128Reader(encoded[4:])
    relocation_count = reader.pop()
    relocation_offset = reader.pop()
    if relocation_count < 0 or relocation_count > MAX_RELOCATIONS:
        raise AnalyzerError("APS2 relocation count is invalid")
    relocation_info = 0
    relocation_addend = 0
    emitted = 0
    while emitted < relocation_count:
        group_size = reader.pop()
        group_flags = reader.pop()
        if group_size <= 0 or group_size > relocation_count - emitted:
            raise AnalyzerError("APS2 relocation group is invalid")
        offset_delta = reader.pop(
        ) if group_flags & APS2_GROUPED_BY_OFFSET_DELTA else 0
        if group_flags & APS2_GROUPED_BY_INFO:
            relocation_info = reader.pop()
        addend_mode = group_flags & (APS2_GROUP_HAS_ADDEND |
                                     APS2_GROUPED_BY_ADDEND)
        if addend_mode == (APS2_GROUP_HAS_ADDEND | APS2_GROUPED_BY_ADDEND):
            relocation_addend += reader.pop()
        elif addend_mode != APS2_GROUP_HAS_ADDEND:
            relocation_addend = 0
        for _ in range(group_size):
            relocation_offset += (offset_delta if group_flags &
                                  APS2_GROUPED_BY_OFFSET_DELTA else
                                  reader.pop())
            if not group_flags & APS2_GROUPED_BY_INFO:
                relocation_info = reader.pop()
            if addend_mode == APS2_GROUP_HAS_ADDEND:
                relocation_addend += reader.pop()
            if (relocation_offset < 0 or relocation_info < 0 or
                    relocation_addend < 0 or relocation_offset >= 1 << 64 or
                    relocation_info >= 1 << 64 or relocation_addend >= 1 << 64):
                raise AnalyzerError(
                    "APS2 relocation exceeds unsigned 64-bit bounds")
            emitted += 1
            yield Relocation(relocation_offset, relocation_info,
                             relocation_addend)
    if not reader.at_end():
        raise AnalyzerError("APS2 relocation stream contains trailing bytes")


class ElfImage:

    def __init__(self, path: pathlib.Path):
        self.path = path
        self._file = None
        self.data = None
        self.file_size = 0
        self.sha256 = ""
        self.sections: dict[str, Section] = {}
        self.segments: tuple[Segment, ...] = ()
        self.relro_ranges: tuple[tuple[int, int], ...] = ()
        self.build_id = ""
        self._relocations: dict[int, int] | None = None

    def __enter__(self) -> "ElfImage":
        try:
            file_status = os.lstat(self.path)
        except OSError as error:
            raise AnalyzerError(f"cannot stat ELF file: {self.path}") from error
        if stat.S_ISLNK(
                file_status.st_mode) or not stat.S_ISREG(file_status.st_mode):
            raise AnalyzerError(
                f"ELF input must be a regular non-symlink: {self.path}")
        if (file_status.st_size < ELF_HEADER.size or
                file_status.st_size > MAX_ELF_BYTES):
            raise AnalyzerError(f"ELF input has an invalid size: {self.path}")
        self.file_size = file_status.st_size
        try:
            self._file = self.path.open("rb")
            self.sha256 = sha256_file(self._file)
            self._file.seek(0)
            self.data = mmap.mmap(self._file.fileno(),
                                  0,
                                  access=mmap.ACCESS_READ)
            self._parse()
        except AnalyzerError:
            self.__exit__(None, None, None)
            raise
        except (OSError, ValueError, struct.error) as error:
            self.__exit__(None, None, None)
            raise AnalyzerError(
                f"cannot parse ELF file: {self.path}") from error
        return self

    def __exit__(self, _type, _value, _traceback) -> None:
        if self.data is not None:
            self.data.close()
            self.data = None
        if self._file is not None:
            self._file.close()
            self._file = None

    def _checked_range(self, offset: int, size: int, description: str) -> None:
        if offset < 0 or size < 0 or offset > self.file_size - size:
            raise AnalyzerError(f"ELF {description} exceeds file bounds")

    def bytes_at(self, offset: int, size: int, description: str) -> bytes:
        self._checked_range(offset, size, description)
        return self.data[offset:offset + size]

    def section_bytes(self, section: Section) -> bytes:
        return self.bytes_at(section.offset, section.size,
                             f"section {section.name}")

    def _parse(self) -> None:
        header = ELF_HEADER.unpack_from(self.data, 0)
        identification = header[0]
        if (identification[:4] != b"\x7fELF" or
                identification[4] != ELF_CLASS_64 or
                identification[5] != ELF_DATA_LITTLE_ENDIAN or
                identification[6] != ELF_VERSION_CURRENT or
                header[1] != ELF_TYPE_SHARED_OBJECT or
                header[2] != ELF_MACHINE_X86_64 or
                header[3] != ELF_VERSION_CURRENT):
            raise AnalyzerError(
                "candidate must be a little-endian x86-64 ET_DYN ELF")

        program_offset = header[5]
        section_offset = header[6]
        program_entry_size = header[9]
        program_count = header[10]
        section_entry_size = header[11]
        section_count = header[12]
        section_names_index = header[13]
        if (program_entry_size != PROGRAM_HEADER.size or
                section_entry_size != SECTION_HEADER.size or
                program_count == 0 or section_count == 0 or
                section_names_index == 0 or
                section_names_index >= section_count):
            raise AnalyzerError("ELF header tables use an unsupported layout")
        self._checked_range(program_offset, program_count * program_entry_size,
                            "program headers")
        self._checked_range(section_offset, section_count * section_entry_size,
                            "section headers")

        segments = []
        relro_ranges = []
        for index in range(program_count):
            values = PROGRAM_HEADER.unpack_from(
                self.data, program_offset + index * program_entry_size)
            if values[0] == PT_GNU_RELRO:
                if values[6] <= 0:
                    raise AnalyzerError("ELF PT_GNU_RELRO range is empty")
                relro_ranges.append((values[3], values[6]))
            if values[0] != PT_LOAD:
                continue
            segment = Segment(
                offset=values[2],
                virtual_address=values[3],
                file_size=values[5],
                memory_size=values[6],
                flags=values[1],
            )
            self._checked_range(segment.offset, segment.file_size,
                                "PT_LOAD data")
            if segment.file_size > segment.memory_size:
                raise AnalyzerError("ELF PT_LOAD file size exceeds memory size")
            segments.append(segment)
        if not segments:
            raise AnalyzerError("ELF has no PT_LOAD segments")
        self.segments = tuple(segments)
        self.relro_ranges = tuple(relro_ranges)

        raw_sections = [
            SECTION_HEADER.unpack_from(
                self.data, section_offset + index * section_entry_size)
            for index in range(section_count)
        ]
        names_header = raw_sections[section_names_index]
        self._checked_range(names_header[4], names_header[5],
                            "section-name table")
        names = self.data[names_header[4]:names_header[4] + names_header[5]]

        by_name: dict[str, Section] = {}
        for values in raw_sections:
            name_offset = values[0]
            if name_offset >= len(names):
                raise AnalyzerError("ELF section name exceeds string table")
            name_end = names.find(b"\0", name_offset)
            if name_end < 0:
                raise AnalyzerError("ELF section name is not terminated")
            try:
                name = names[name_offset:name_end].decode("ascii")
            except UnicodeDecodeError as error:
                raise AnalyzerError("ELF section name is not ASCII") from error
            section = Section(
                name=name,
                section_type=values[1],
                flags=values[2],
                address=values[3],
                offset=values[4],
                size=values[5],
            )
            if section.section_type != SHT_NOBITS:
                self._checked_range(section.offset, section.size,
                                    f"section {name}")
            if name and name in by_name:
                raise AnalyzerError(f"ELF contains duplicate section {name}")
            if name:
                by_name[name] = section
        self.sections = by_name

        text = self._require_section(".text", SHT_PROGBITS)
        if not text.flags & 2 or not text.flags & 4 or text.flags & 1:
            raise AnalyzerError(
                "ELF .text section must be read+execute, non-writable")
        self._require_section(".rela.dyn", SHT_ANDROID_RELA)
        self._require_section(".eh_frame_hdr", SHT_PROGBITS)
        self._require_section(".note.gnu.build-id", SHT_NOTE)
        self.build_id = self._read_build_id()
        if len(self.build_id) != 40:
            raise AnalyzerError("ELF GNU build ID is not a 20-byte SHA-1")

    def _require_section(self, name: str, section_type: int) -> Section:
        section = self.sections.get(name)
        if section is None or section.section_type != section_type:
            raise AnalyzerError(f"ELF is missing required {name} section")
        return section

    def _read_build_id(self) -> str:
        section = self.sections[".note.gnu.build-id"]
        encoded = self.section_bytes(section)
        offset = 0
        while offset + NOTE_HEADER.size <= len(encoded):
            namesz, descsz, note_type = NOTE_HEADER.unpack_from(encoded, offset)
            offset += NOTE_HEADER.size
            if offset + namesz > len(encoded):
                raise AnalyzerError("GNU build-id note name is truncated")
            note_name = encoded[offset:offset + namesz]
            offset += namesz
            offset += (-offset) % 4
            if note_type == NT_GNU_BUILD_ID and note_name.rstrip(
                    b"\0") == b"GNU":
                if offset + descsz > len(encoded):
                    raise AnalyzerError("GNU build-id note data is truncated")
                return encoded[offset:offset + descsz].hex()
            offset += descsz
            offset += (-offset) % 4
        raise AnalyzerError("ELF has no GNU build-id note")

    def segment_for_rva(self, rva: int, size: int = 1) -> Segment | None:
        matches = [
            segment for segment in self.segments
            if segment.contains_memory(rva, size)
        ]
        return matches[0] if len(matches) == 1 else None

    def rva_to_offset(self, rva: int, size: int = 1) -> int:
        matches = [
            segment for segment in self.segments
            if segment.contains_file_data(rva, size)
        ]
        if len(matches) != 1:
            raise AnalyzerError(f"RVA 0x{rva:x} has no unique file mapping")
        segment = matches[0]
        offset = segment.offset + rva - segment.virtual_address
        self._checked_range(offset, size, "RVA mapping")
        return offset

    def offset_to_rva(self, offset: int) -> int | None:
        for segment in self.segments:
            if segment.offset <= offset < segment.offset + segment.file_size:
                return offset - segment.offset + segment.virtual_address
        return None

    def require_code_rva(self, rva: int, size: int = 1) -> None:
        segment = self.segment_for_rva(rva, size)
        if (segment is None or segment.flags &
            (PF_READ | PF_EXECUTE) != (PF_READ | PF_EXECUTE) or
                segment.flags & PF_WRITE or
                not segment.contains_file_data(rva, size)):
            raise AnalyzerError(
                f"RVA 0x{rva:x} is not in a read/execute segment")

    def require_writable_rva(self, rva: int, size: int = 1) -> None:
        segment = self.segment_for_rva(rva, size)
        if (segment is None or
                segment.flags & (PF_READ | PF_WRITE) != (PF_READ | PF_WRITE) or
                segment.flags & PF_EXECUTE):
            raise AnalyzerError(
                f"RVA 0x{rva:x} is not in a non-executable RW segment")

    def require_relro_rva(self, rva: int, size: int = 1) -> None:
        matches = [
            (begin, length)
            for begin, length in self.relro_ranges
            if size >= 0 and begin <= rva and rva + size <= begin + length
        ]
        if len(matches) != 1:
            raise AnalyzerError(
                f"RVA 0x{rva:x} is not in a unique PT_GNU_RELRO range")

    def text_section(self) -> Section:
        return self.sections[".text"]

    def find_alloc_string(self, needle: bytes) -> int:
        """Return the unique read-only mapped RVA whose bytes equal needle."""
        if b"\0" in needle:
            raise AnalyzerError("anchor strings must not contain NUL bytes")
        wanted = needle + b"\0"
        hits: list[int] = []
        position = 0
        while True:
            position = self.data.find(wanted, position)
            if position < 0:
                break
            rva = self.offset_to_rva(position)
            if rva is not None:
                segment = self.segment_for_rva(rva, len(wanted))
                if segment is not None and not segment.flags & PF_WRITE:
                    hits.append(rva)
            position += 1
        if not hits:
            raise AnalyzerError(f"anchor string {needle.decode('ascii')!r}"
                                " was not found in the ELF")
        if len(hits) > 1:
            listed = ", ".join(f"0x{hit:x}" for hit in hits)
            raise AnalyzerError(f"anchor string {needle.decode('ascii')!r}"
                                f" is ambiguous: {listed}")
        return hits[0]

    def relative_relocations(self) -> dict[int, int]:
        if self._relocations is not None:
            return self._relocations
        section = self.sections[".rela.dyn"]
        if section.size > MAX_RELA_DYN_BYTES:
            raise AnalyzerError(".rela.dyn section is suspiciously large")
        encoded = self.section_bytes(section)
        relocations: dict[int, int] = {}
        count = 0
        for relocation in decode_aps2_relocations(encoded):
            count += 1
            if count > MAX_RELOCATIONS:
                raise AnalyzerError("APS2 relocation stream is too large")
            if relocation.info & 0xFFFFFFFF in (
                    R_X86_64_RELATIVE,
                    R_X86_64_IRELATIVE,
            ):
                relocations[relocation.offset] = relocation.addend
        self._relocations = relocations
        return relocations


class FunctionTable:
    """Function boundaries from the .eh_frame_hdr binary-search table."""

    def __init__(self, image: ElfImage):
        self.image = image
        section = image.sections[".eh_frame_hdr"]
        encoded = image.section_bytes(section)
        base = section.address
        if len(encoded) < 12 or encoded[:4] != bytes((1, 27, 3, 59)):
            raise AnalyzerError(".eh_frame_hdr uses an unsupported encoding")
        count = struct.unpack_from("<I", encoded, 8)[0]
        if count <= 0 or count > MAX_FUNCTION_TABLE_ENTRIES:
            raise AnalyzerError(".eh_frame_hdr table size is invalid")
        if len(encoded) < 12 + count * 8:
            raise AnalyzerError(".eh_frame_hdr table is truncated")
        self.starts = [
            base + struct.unpack_from("<i", encoded, 12 + index * 8)[0]
            for index in range(count)
        ]
        if any(self.starts[i] >= self.starts[i + 1] for i in range(count - 1)):
            raise AnalyzerError(".eh_frame_hdr table is not sorted")
        text = image.text_section()
        self.ends = self.starts[1:] + [text.address + text.size]

    def bounds(self, rva: int) -> tuple[int, int]:
        index = bisect.bisect_right(self.starts, rva) - 1
        if index < 0 or self.starts[index] > rva or rva >= self.ends[index]:
            raise AnalyzerError(
                f"RVA 0x{rva:x} is not covered by .eh_frame_hdr")
        return self.starts[index], self.ends[index]


class Disassembler:

    def __init__(self, image: ElfImage):
        require_capstone()
        self.image = image
        self.decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.decoder.detail = True
        self._cache: dict[int, tuple[Any, ...]] = {}

    def function(self, start: int, end: int) -> tuple[Any, ...]:
        cached = self._cache.get(start)
        if cached is not None:
            return cached
        size = end - start
        if size <= 0 or size > MAX_FUNCTION_BYTES:
            raise AnalyzerError(f"function at 0x{start:x} has an invalid size")
        offset = self.image.rva_to_offset(start, size)
        code = self.image.bytes_at(offset, size, "function body")
        instructions = tuple(self.decoder.disasm(code, start))
        if not instructions or instructions[0].address != start:
            raise AnalyzerError(f"cannot decode function at 0x{start:x}")
        self._cache[start] = instructions
        return instructions


def require_capstone() -> None:
    if capstone is None or capstone_x86 is None:
        raise AnalyzerError(
            "Python capstone 5 is required for VR offset analysis")
    try:
        version = capstone.cs_version()
    except (AttributeError, TypeError, ValueError) as error:
        raise AnalyzerError(
            "Python capstone version cannot be verified") from error
    if (not isinstance(version, tuple) or len(version) < 2 or
            not all(isinstance(component, int) for component in version[:2]) or
            version[0] != 5):
        raise AnalyzerError("Python capstone major version 5 is required")


def rip_target(instruction: Any) -> Iterator[int]:
    for operand in instruction.operands:
        if (operand.type == capstone_x86.X86_OP_MEM and
                operand.mem.base == capstone_x86.X86_REG_RIP):
            yield instruction.address + instruction.size + operand.mem.disp


def find_text_refs(
    image: ElfImage,
    table: FunctionTable,
    disasm: Disassembler,
    targets: set[int],
) -> dict[int, list[tuple[int, int, int]]]:
    """Find RIP-relative references at decoded instruction boundaries.

    Returns:
        Target RVAs mapped to (instruction RVA, function start, function end).
    """
    text = image.text_section()
    data = image.section_bytes(text)
    base = text.address
    combined = re.compile(
        b"(?=" + TEXT_REF_OPCODES.pattern + TEXT_REF_MODRMS.pattern + b".{4})",
        re.DOTALL,
    )
    candidates: dict[int, set[int]] = {}
    for match in combined.finditer(data):
        position = match.start()
        opcode = data[position]
        extra = 1 if opcode in (0x80, 0x83,
                                0xC6) else 4 if opcode in (0x81, 0xC7) else 0
        target = (base + position + 6 + extra +
                  struct.unpack_from("<i", data, position + 2)[0])
        if target in targets:
            candidates.setdefault(target, set()).add(base + position)
    results: dict[int, list[tuple[int, int, int]]] = {}
    for target in sorted(candidates):
        for position in sorted(candidates[target]):
            start, end = table.bounds(position)
            instructions = disasm.function(start, end)
            for instruction in instructions:
                if (instruction.address <= position <
                        instruction.address + instruction.size):
                    if target in rip_target(instruction):
                        results.setdefault(target, []).append(
                            (instruction.address, start, end))
    return results


def scan_text_bytes(image: ElfImage, pattern: bytes) -> list[int]:
    text = image.text_section()
    data = image.section_bytes(text)
    base = text.address
    hits: list[int] = []
    position = 0
    while True:
        position = data.find(pattern, position)
        if position < 0:
            return hits
        hits.append(base + position)
        position += 1


def ends_with_ret(instructions: Sequence[Any]) -> bool:
    return any(
        instruction.mnemonic == "ret" for instruction in instructions[-3:])


def is_state_getter(instructions: Sequence[Any]) -> bool:
    """mov reg, rdi; add rsi, 0x14; mov edx, 0x138; call memcpy; ret."""
    if not 6 <= len(instructions) <= 24:
        return False
    calls = [
        instruction for instruction in instructions
        if instruction.mnemonic == "call" and instruction.operands and
        instruction.operands[0].type == capstone_x86.X86_OP_IMM
    ]
    if len(calls) != 1:
        return False
    if not ends_with_ret(instructions):
        return False
    has_saved_self = False
    has_state_offset = False
    has_state_size = False
    for instruction in instructions:
        operands = instruction.operands
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[1].type == capstone_x86.X86_OP_REG and
                operands[1].reg == capstone_x86.X86_REG_RDI):
            has_saved_self = True
        if (instruction.mnemonic == "add" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[0].reg == capstone_x86.X86_REG_RSI and
                operands[1].type == capstone_x86.X86_OP_IMM and
                operands[1].imm == STATE_OFFSET):
            has_state_offset = True
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[1].type == capstone_x86.X86_OP_IMM and
                operands[1].imm == STATE_COPY_SIZE):
            has_state_size = True
    return has_saved_self and has_state_offset and has_state_size


def is_eye_getter(instructions: Sequence[Any]) -> bool:
    """movsxd rax, esi; shl rax, 4; mov rax, [rdi+rax+0x150]; ret."""
    if len(instructions) != 4:
        return False
    first, second, third, last = instructions
    if first.mnemonic != "movsxd" or len(first.operands) != 2:
        return False
    if (first.operands[0].type != capstone_x86.X86_OP_REG or
            first.operands[0].reg != capstone_x86.X86_REG_RAX or
            first.operands[1].type != capstone_x86.X86_OP_REG or
            first.operands[1].reg != capstone_x86.X86_REG_ESI):
        return False
    if (second.mnemonic != "shl" or
            second.operands[0].type != capstone_x86.X86_OP_REG or
            second.operands[0].reg != capstone_x86.X86_REG_RAX or
            second.operands[1].type != capstone_x86.X86_OP_IMM or
            second.operands[1].imm != 4):
        return False
    if third.mnemonic != "mov" or len(third.operands) != 2:
        return False
    memory = third.operands[1]
    if memory.type != capstone_x86.X86_OP_MEM:
        return False
    if (memory.mem.base != capstone_x86.X86_REG_RDI or
            memory.mem.index != capstone_x86.X86_REG_RAX or
            memory.mem.disp != FRAMEBUFFER_SLOT_OFFSET):
        return False
    return last.mnemonic == "ret"


def is_eye_initializer(instructions: Sequence[Any], eye_label_rva: int) -> bool:
    """The eye-resource initializer references 'VR Eye' and the slot arrays."""
    has_label = any(eye_label_rva in rip_target(instruction)
                    for instruction in instructions)
    if not has_label:
        return False
    ready_hits = set()
    has_framebuffer_lea = False
    has_texture_lea = False
    has_register_call = False
    for instruction in instructions:
        operands = instruction.operands
        if instruction.mnemonic == "mov" and operands and operands[
                0].type == capstone_x86.X86_OP_MEM:
            if operands[0].size == 1 and operands[
                    0].mem.disp in READY_BYTE_OFFSETS:
                ready_hits.add(operands[0].mem.disp)
        if instruction.mnemonic == "lea" and len(operands) == 2:
            memory = operands[1]
            if (memory.type == capstone_x86.X86_OP_MEM and
                    memory.mem.index == capstone_x86.X86_REG_INVALID):
                if memory.mem.disp == FRAMEBUFFER_SLOT_OFFSET:
                    has_framebuffer_lea = True
                if memory.mem.disp == TEXTURE_SLOT_OFFSET:
                    has_texture_lea = True
        if (instruction.mnemonic == "call" and operands and
                operands[0].type == capstone_x86.X86_OP_REG):
            has_register_call = True
    return (len(ready_hits) >= 2 and has_framebuffer_lea and has_texture_lea and
            has_register_call)


def is_constructor(instructions: Sequence[Any], vtable_rva: int,
                   device_name_rva: int) -> bool:
    """The constructor stores the vtable, the 'DebugDevice' name and type 6."""
    has_vtable = any(
        instruction.mnemonic == "lea" and vtable_rva in rip_target(instruction)
        for instruction in instructions)
    has_name = any(instruction.mnemonic == "lea" and
                   device_name_rva in rip_target(instruction)
                   for instruction in instructions)
    has_type = False
    has_state_size = False
    has_direct_call = False
    for instruction in instructions:
        operands = instruction.operands
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_MEM and
                operands[0].size == 4 and
                operands[0].mem.disp == OBJECT_TYPE_OFFSET and
                operands[0].mem.index == capstone_x86.X86_REG_INVALID and
                operands[1].type == capstone_x86.X86_OP_IMM and
                operands[1].imm == OBJECT_TYPE_DEBUG_DEVICE):
            has_type = True
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[1].type == capstone_x86.X86_OP_IMM and
                operands[1].imm == CONSTRUCTOR_MEMSET_SIZE):
            has_state_size = True
        if (instruction.mnemonic == "call" and operands and
                operands[0].type == capstone_x86.X86_OP_IMM):
            has_direct_call = True
    return (has_vtable and has_name and has_type and has_state_size and
            has_direct_call)


def extract_framebuffer_slot(instructions: Sequence[Any]) -> int | None:
    """Find ``mov rax, [rax+disp]`` ... ``call rax`` on a device vtable."""
    hits: set[int] = set()
    for index, instruction in enumerate(instructions):
        operands = instruction.operands
        if instruction.mnemonic != "mov" or len(operands) != 2:
            continue
        if operands[0].type != capstone_x86.X86_OP_REG or operands[
                1].type != capstone_x86.X86_OP_MEM:
            continue
        memory = operands[1].mem
        register = operands[0].reg
        if (memory.base != register or
                memory.index != capstone_x86.X86_REG_INVALID or
                memory.disp < 0x40):
            continue
        has_vtable_load = any(
            previous.mnemonic == "mov" and len(previous.operands) == 2 and
            previous.operands[0].type == capstone_x86.X86_OP_REG and
            previous.operands[1].type == capstone_x86.X86_OP_MEM and
            previous.operands[1].mem.index == capstone_x86.X86_REG_INVALID and
            previous.operands[1].mem.disp == 0 and
            previous.operands[1].mem.base != capstone_x86.X86_REG_INVALID and
            previous.operands[1].mem.base != capstone_x86.X86_REG_RIP
            for previous in instructions[max(0, index - 6):index])
        has_register_call = any(
            later.mnemonic == "call" and later.operands and
            later.operands[0].type == capstone_x86.X86_OP_REG and
            later.operands[0].reg == register
            for later in instructions[index + 1:index + 16])
        if has_vtable_load and has_register_call:
            hits.add(memory.disp)
    if len(hits) != 1:
        return None
    return next(iter(hits))


def extract_registration_storage(instructions: Sequence[Any],
                                 flag_name_rva: int) -> int | None:
    """lea rsi, [flag]; lea rdx, [storage]; mov ecx, 1; mov r8d, 4; jmp."""
    storage: int | None = None
    for index, instruction in enumerate(instructions):
        operands = instruction.operands
        if instruction.mnemonic != "lea" or len(operands) != 2:
            continue
        if operands[0].type != capstone_x86.X86_OP_REG or operands[
                0].reg != capstone_x86.X86_REG_RSI:
            continue
        if flag_name_rva not in rip_target(instruction):
            continue
        for follower in instructions[index + 1:index + 4]:
            follower_operands = follower.operands
            if (follower.mnemonic == "lea" and len(follower_operands) == 2 and
                    follower_operands[0].type == capstone_x86.X86_OP_REG and
                    follower_operands[0].reg == capstone_x86.X86_REG_RDX):
                for target in rip_target(follower):
                    storage = target
    if storage is None:
        return None
    has_flag_scope = False
    has_category = False
    for instruction in instructions:
        operands = instruction.operands
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[0].reg == capstone_x86.X86_REG_ECX and
                operands[1].type == capstone_x86.X86_OP_IMM and
                operands[1].imm == 1):
            has_flag_scope = True
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[0].reg == capstone_x86.X86_REG_R8D and
                operands[1].type == capstone_x86.X86_OP_IMM and
                operands[1].imm == 4):
            has_category = True
    if not (has_flag_scope and has_category):
        return None
    return storage


def find_flag_zeroing(instructions: Sequence[Any], storage_rva: int) -> bool:
    """mov byte ptr [rip+storage], 0 in the flag descriptor initializer."""
    for instruction in instructions:
        operands = instruction.operands
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_MEM and
                operands[0].size == 1 and
                operands[1].type == capstone_x86.X86_OP_IMM and
                operands[1].imm == 0):
            if storage_rva in rip_target(instruction):
                return True
    return False


def extract_hand_pitch(instructions: Sequence[Any],
                       flag_name_rva: int) -> tuple[int, int] | None:
    """Find hand-pitch storage and its default value in the flag initializer."""
    for index, instruction in enumerate(instructions):
        operands = instruction.operands
        if (instruction.mnemonic != "mov" or len(operands) != 2 or
                operands[0].type != capstone_x86.X86_OP_MEM or
                operands[1].type != capstone_x86.X86_OP_IMM):
            continue
        if operands[0].size != 4:
            continue
        default = operands[1].imm
        if not 0 <= default < 360:
            continue
        storage = next(iter(rip_target(instruction)), None)
        if storage is None:
            continue
        for follower_index in range(index + 1, min(index + 5,
                                                   len(instructions))):
            follower = instructions[follower_index]
            follower_operands = follower.operands
            if (follower.mnemonic != "lea" or len(follower_operands) != 2 or
                    follower_operands[0].type != capstone_x86.X86_OP_REG):
                continue
            if flag_name_rva not in rip_target(follower):
                continue
            register = follower_operands[0].reg
            if follower_index + 1 >= len(instructions):
                continue
            store = instructions[follower_index + 1]
            store_operands = store.operands
            if (store.mnemonic == "mov" and len(store_operands) == 2 and
                    store_operands[0].type == capstone_x86.X86_OP_MEM and
                    store_operands[1].type == capstone_x86.X86_OP_REG and
                    store_operands[1].reg == register):
                name_slot = next(iter(rip_target(store)), None)
                if name_slot == storage + 8:
                    return storage, default
    return None


def match_pointer_getter(instructions: Sequence[Any]) -> bool:
    """World-space pointer getter: UserCFrame copy + workspace/camera path."""
    has_selector = False
    has_scale = False
    has_stride = False
    usercframe_loads = 0
    has_workspace_handoff = False
    has_camera_slot_call = False
    has_head_scale = False
    for instruction in instructions:
        operands = instruction.operands
        if instruction.mnemonic == "movsxd" and len(operands) == 2:
            if (operands[1].type == capstone_x86.X86_OP_MEM and
                    operands[1].mem.base == capstone_x86.X86_REG_RSI and
                    operands[1].mem.index == capstone_x86.X86_REG_INVALID and
                    operands[1].mem.disp == USERCFRAME_SELECTOR_OFFSET):
                has_selector = True
        if instruction.mnemonic == "lea" and len(operands) == 2:
            memory = operands[1]
            if (memory.type == capstone_x86.X86_OP_MEM and
                    operands[0].type == capstone_x86.X86_OP_REG and
                    operands[0].reg == capstone_x86.X86_REG_RAX and
                    memory.mem.base == capstone_x86.X86_REG_RAX and
                    memory.mem.index == capstone_x86.X86_REG_RAX and
                    memory.mem.scale == 2):
                has_scale = True
        if instruction.mnemonic == "shl" and len(operands) == 2:
            if (operands[0].type == capstone_x86.X86_OP_REG and
                    operands[0].reg == capstone_x86.X86_REG_RAX and
                    operands[1].type == capstone_x86.X86_OP_IMM and
                    operands[1].imm == 4):
                has_stride = True
        if instruction.mnemonic in ("movups", "movupd", "movaps",
                                    "movapd") and len(operands) == 2:
            memory = operands[1]
            if memory.type == capstone_x86.X86_OP_MEM and (
                    memory.mem.base == capstone_x86.X86_REG_RSI and
                    memory.mem.index == capstone_x86.X86_REG_RAX and
                    memory.mem.disp in (
                        USERCFRAME_BASE_OFFSET,
                        USERCFRAME_BASE_OFFSET + 0x10,
                        USERCFRAME_BASE_OFFSET + 0x20,
                    )):
                usercframe_loads += 1
        if instruction.mnemonic == "movss" and len(operands) == 2:
            memory = operands[1]
            if (memory.type == capstone_x86.X86_OP_MEM and
                    memory.mem.index == capstone_x86.X86_REG_INVALID and
                    memory.mem.disp == CAMERA_HEAD_SCALE_OFFSET):
                has_head_scale = True
    for index, instruction in enumerate(instructions):
        operands = instruction.operands
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[0].reg == capstone_x86.X86_REG_RDI and
                operands[1].type == capstone_x86.X86_OP_REG and
                operands[1].reg == capstone_x86.X86_REG_RSI):
            for follower in instructions[index + 1:index + 3]:
                if (follower.mnemonic == "call" and follower.operands and
                        follower.operands[0].type == capstone_x86.X86_OP_IMM):
                    has_workspace_handoff = True
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[0].reg == capstone_x86.X86_REG_RCX and
                operands[1].type == capstone_x86.X86_OP_MEM and
                operands[1].mem.base == capstone_x86.X86_REG_RAX and
                operands[1].mem.disp == 0):
            for follower_index in range(index + 1,
                                        min(index + 4, len(instructions))):
                follower = instructions[follower_index]
                if (follower.mnemonic == "mov" and
                        len(follower.operands) == 2 and
                        follower.operands[0].type == capstone_x86.X86_OP_REG and
                        follower.operands[0].reg == capstone_x86.X86_REG_RDI and
                        follower.operands[1].type == capstone_x86.X86_OP_REG and
                        follower.operands[1].reg == capstone_x86.X86_REG_RAX):
                    for caller in instructions[follower_index +
                                               1:follower_index + 4]:
                        if (caller.mnemonic == "call" and caller.operands and
                                caller.operands[0].type
                                == capstone_x86.X86_OP_MEM and
                                caller.operands[0].mem.base
                                == capstone_x86.X86_REG_RCX and
                                caller.operands[0].mem.disp > 0x100):
                            has_camera_slot_call = True
    return (has_selector and has_scale and has_stride and
            usercframe_loads >= 3 and has_workspace_handoff and
            has_camera_slot_call and has_head_scale)


def extract_pointer_getter_anchors(instructions: Sequence[Any],
                                   image: ElfImage) -> tuple[int, int]:
    """Return (workspace getter RVA, camera vtable slot offset)."""
    workspace_getter = None
    camera_slot = None
    for index, instruction in enumerate(instructions):
        operands = instruction.operands
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[0].reg == capstone_x86.X86_REG_RDI and
                operands[1].type == capstone_x86.X86_OP_REG and
                operands[1].reg == capstone_x86.X86_REG_RSI):
            for follower in instructions[index + 1:index + 3]:
                if (follower.mnemonic == "call" and follower.operands and
                        follower.operands[0].type == capstone_x86.X86_OP_IMM):
                    target = follower.operands[0].imm
                    try:
                        image.require_code_rva(target)
                    except AnalyzerError:
                        continue
                    workspace_getter = target
        if (instruction.mnemonic == "mov" and len(operands) == 2 and
                operands[0].type == capstone_x86.X86_OP_REG and
                operands[0].reg == capstone_x86.X86_REG_RCX and
                operands[1].type == capstone_x86.X86_OP_MEM and
                operands[1].mem.base == capstone_x86.X86_REG_RAX and
                operands[1].mem.disp == 0):
            for follower_index in range(index + 1,
                                        min(index + 4, len(instructions))):
                follower = instructions[follower_index]
                if (follower.mnemonic == "mov" and
                        len(follower.operands) == 2 and
                        follower.operands[0].type == capstone_x86.X86_OP_REG and
                        follower.operands[0].reg == capstone_x86.X86_REG_RDI and
                        follower.operands[1].type == capstone_x86.X86_OP_REG and
                        follower.operands[1].reg == capstone_x86.X86_REG_RAX):
                    for caller in instructions[follower_index +
                                               1:follower_index + 4]:
                        if (caller.mnemonic == "call" and caller.operands and
                                caller.operands[0].type
                                == capstone_x86.X86_OP_MEM and
                                caller.operands[0].mem.base
                                == capstone_x86.X86_REG_RCX and
                                caller.operands[0].mem.disp > 0x100):
                            camera_slot = caller.operands[0].mem.disp
    if workspace_getter is None or camera_slot is None:
        raise AnalyzerError("world-space pointer getter contract matched"
                            " but its workspace/camera"
                            " anchors could not be extracted")
    return workspace_getter, camera_slot


def vtable_slots(
    image: ElfImage,
    relocations: dict[int, int],
    vtable_rva: int,
    table: "FunctionTable",
) -> dict[int, int]:
    """Collect vtable slots whose targets are covered by the function table.

    Segment flags alone cannot distinguish .text from .rodata in these builds.
    """
    slots: dict[int, int] = {}
    misses = 0
    for slot in range(0, MAX_VTABLE_SCAN_BYTES, 8):
        target = relocations.get(vtable_rva + slot)
        valid = False
        if target is not None:
            try:
                image.require_code_rva(target)
                table.bounds(target)
                valid = True
            except AnalyzerError:
                valid = False
        if valid:
            slots[slot] = target
            misses = 0
        else:
            misses += 1
            if misses >= 8:
                break
    if len(slots) < 16:
        raise AnalyzerError(
            f"vtable candidate at 0x{vtable_rva:x} does not look like a vtable")
    return slots


def find_vr_offsets(elf_path: pathlib.Path) -> dict[str, Any]:
    """Analyze VR entry points without executing the ELF.

    Args:
        elf_path: Path to an x86-64 libroblox.so.

    Returns:
        ELF identity, bridge offsets, evidence, and analysis warnings.

    Raises:
        AnalyzerError: The ELF or a required machine-code contract is invalid.
    """
    evidence: list[str] = []
    warnings: list[str] = []

    with ElfImage(elf_path) as image:
        relocations = image.relative_relocations()
        table = FunctionTable(image)
        disasm = Disassembler(image)

        # Itanium RTTI: mangled name -> typeinfo -> primary vtable.
        rtti_name_rva = image.find_alloc_string(RTTI_TYPE_NAME)
        name_slots = sorted(offset for offset, addend in relocations.items()
                            if addend == rtti_name_rva and offset % 8 == 0)
        if not name_slots:
            raise AnalyzerError("no APS2 relative relocation references"
                                " the DebugDeviceVR RTTI name")
        vtable_result: tuple[int, dict[int, int]] | None = None
        typeinfo_result = 0
        for name_slot in name_slots:
            typeinfo = name_slot - 8
            for reference in sorted(
                    offset for offset, addend in relocations.items()
                    if addend == typeinfo and offset % 8 == 0):
                vtable = reference + 8
                try:
                    image.require_relro_rva(vtable, 0xE0)
                    slots = vtable_slots(image, relocations, vtable, table)
                except AnalyzerError:
                    continue
                vtable_result = (vtable, slots)
                typeinfo_result = typeinfo
                break
            if vtable_result is not None:
                break
        if vtable_result is None:
            raise AnalyzerError(
                "DebugDeviceVR vtable could not be resolved from its typeinfo")
        vtable, slots = vtable_result
        offset_to_top = image.bytes_at(image.rva_to_offset(vtable - 16, 8), 8,
                                       "vtable header")
        if offset_to_top != b"\0" * 8:
            raise AnalyzerError(
                f"vtable at 0x{vtable:x} is not a primary vtable (offset-to-top"
                " is nonzero)")
        evidence.append(
            f"RTTI: mangled name {RTTI_TYPE_NAME.decode('ascii')!r} at"
            f" 0x{rtti_name_rva:x} -> typeinfo 0x{typeinfo_result:x} -> vtable"
            f" 0x{vtable:x} through APS2 relative relocations")

        state_matches = [
            (slot, target)
            for slot, target in slots.items()
            if is_state_getter(disasm.function(*table.bounds(target)))
        ]
        eye_matches = [(slot, target)
                       for slot, target in slots.items()
                       if is_eye_getter(disasm.function(*table.bounds(target)))]
        eye_label_rva = image.find_alloc_string(EYE_RESOURCE_LABEL)
        initializer_matches = [
            (slot, target)
            for slot, target in slots.items()
            if is_eye_initializer(disasm.function(
                *table.bounds(target)), eye_label_rva)
        ]
        for label, matches in (
            ("state getter", state_matches),
            ("eye getter", eye_matches),
            ("eye initializer", initializer_matches),
        ):
            if not matches:
                raise AnalyzerError(
                    f"no vtable slot satisfies the {label} contract")
            if len(matches) > 1:
                listed = ", ".join(f"vtable+0x{slot:x} -> 0x{target:x}"
                                   for slot, target in matches)
                raise AnalyzerError(
                    f"ambiguous {label} contract matches: {listed}")
        state_slot, state_getter = state_matches[0]
        eye_slot, eye_getter = eye_matches[0]
        initializer_slot, eye_initializer = initializer_matches[0]
        for label, slot, expected in (
            ("state getter", state_slot, EXPECTED_STATE_GETTER_SLOT),
            ("eye getter", eye_slot, EXPECTED_EYE_GETTER_SLOT),
            ("eye initializer", initializer_slot,
             EXPECTED_EYE_INITIALIZER_SLOT),
        ):
            if slot != expected:
                warnings.append(f"{label} moved from vtable+0x{expected:x}"
                                f" to vtable+0x{slot:x};"
                                " update the bridge slot constants")
        evidence.append(
            f"state getter: sret memcpy of 0x{STATE_COPY_SIZE:x} bytes from"
            f" +0x{STATE_OFFSET:x} at vtable+0x{state_slot:x}"
            f" -> 0x{state_getter:x}")
        evidence.append("eye getter: movsxd/shl/mov"
                        f" [rdi+rax+0x{FRAMEBUFFER_SLOT_OFFSET:x}] at"
                        f" vtable+0x{eye_slot:x} -> 0x{eye_getter:x}")
        evidence.append(
            f"eye initializer: {EYE_RESOURCE_LABEL.decode('ascii')!r}"
            " label, ready"
            f" bytes {[f'0x{x:x}' for x in READY_BYTE_OFFSETS]}, slot arrays"
            f" +0x{FRAMEBUFFER_SLOT_OFFSET:x}/+0x{TEXTURE_SLOT_OFFSET:x} at"
            f" vtable+0x{initializer_slot:x} -> 0x{eye_initializer:x}")

        haptic_matches = []
        for slot, target in slots.items():
            start, end = table.bounds(target)
            if end - start == 1:
                body = image.bytes_at(image.rva_to_offset(target, 1), 1,
                                      "haptic sink")
                if body == b"\xc3":
                    haptic_matches.append((slot, target))
        if not haptic_matches:
            raise AnalyzerError(
                "no vtable slot points at a 1-byte ret haptic sink")
        haptic_slot, haptic_sink = min(haptic_matches)
        if haptic_slot != EXPECTED_HAPTIC_SLOT:
            warnings.append(
                f"haptic sink moved from vtable+0x{EXPECTED_HAPTIC_SLOT:x} to"
                f" vtable+0x{haptic_slot:x}")
        evidence.append(
            f"haptic sink: 1-byte ret stub at vtable+0x{haptic_slot:x} ->"
            f" 0x{haptic_sink:x}")

        device_name_rva = image.find_alloc_string(DEVICE_OBJECT_NAME)
        vtable_refs = find_text_refs(image, table, disasm, {vtable})
        constructor_candidates: dict[int, int] = {}
        for _target, refs in vtable_refs.items():
            for _address, start, end in refs:
                instructions = disasm.function(start, end)
                if is_constructor(instructions, vtable, device_name_rva):
                    constructor_candidates[start] = end
        if not constructor_candidates:
            raise AnalyzerError(
                "no function satisfies the DebugDeviceVR constructor contract")
        if len(constructor_candidates) > 1:
            listed = ", ".join(
                f"0x{start:x}" for start in constructor_candidates)
            raise AnalyzerError(
                f"ambiguous constructor contract matches: {listed}")
        constructor = next(iter(constructor_candidates))
        if constructor in slots.values():
            raise AnalyzerError(
                "constructor candidate is itself a vtable slot target"
                " (destructor?)")
        evidence.append(
            f"constructor: vtable store, {DEVICE_OBJECT_NAME.decode('ascii')!r}"
            f" name at +0x8, type {OBJECT_TYPE_DEBUG_DEVICE} at"
            f" +0x{OBJECT_TYPE_OFFSET:x},"
            f" memset 0x{CONSTRUCTOR_MEMSET_SIZE:x} of"
            f" +0x{STATE_OFFSET:x} -> 0x{constructor:x}")

        initializer_instructions = disasm.function(
            *table.bounds(eye_initializer))
        framebuffer_slot = extract_framebuffer_slot(initializer_instructions)
        if framebuffer_slot is None:
            raise AnalyzerError(
                "eye initializer does not contain a unique device vtable"
                " framebuffer call")
        evidence.append(
            f"device createFramebuffer: mov rax,[rax+0x{framebuffer_slot:x}];"
            " call rax inside the eye initializer")

        emulator_name_rva = image.find_alloc_string(EMULATOR_FLAG_NAME)
        flag_refs = find_text_refs(image, table, disasm, {emulator_name_rva})
        flag_functions: dict[int, int] = {}
        for _target, refs in flag_refs.items():
            for _address, start, end in refs:
                flag_functions[start] = end
        registration_storage = None
        for start, end in sorted(flag_functions.items()):
            storage = extract_registration_storage(disasm.function(start, end),
                                                   emulator_name_rva)
            if storage is not None:
                if (registration_storage is not None and
                        storage != registration_storage):
                    raise AnalyzerError("ambiguous emulator flag storage:"
                                        f" 0x{registration_storage:x}"
                                        f" and 0x{storage:x}")
                registration_storage = storage
        if registration_storage is None:
            raise AnalyzerError(
                "DebugEnableVREmulator registration contract was not found")
        image.require_writable_rva(registration_storage, 1)
        zeroing_seen = any(
            find_flag_zeroing(disasm.function(start, end), registration_storage)
            for start, end in flag_functions.items())
        if zeroing_seen:
            evidence.append(
                "emulator flag: registration lea rsi/rdx contract plus init"
                f" byte-store cross-check -> 0x{registration_storage:x}")
        else:
            warnings.append("emulator flag storage derived from"
                            " the registration contract only;"
                            " the zero-initializing writer was not found")

        hand_pitch_name_rva = image.find_alloc_string(HAND_PITCH_FLAG_NAME)
        pitch_refs = find_text_refs(image, table, disasm, {hand_pitch_name_rva})
        pitch_functions: dict[int, int] = {}
        for _target, refs in pitch_refs.items():
            for _address, start, end in refs:
                pitch_functions[start] = end
        hand_pitch = None
        for start, end in sorted(pitch_functions.items()):
            match = extract_hand_pitch(disasm.function(start, end),
                                       hand_pitch_name_rva)
            if match is not None:
                if hand_pitch is not None and match != hand_pitch:
                    raise AnalyzerError(
                        "ambiguous NewVRSystemHandPitch storage:"
                        f" 0x{hand_pitch[0]:x}"
                        f" and 0x{match[0]:x}")
                hand_pitch = match
        if hand_pitch is None:
            raise AnalyzerError(
                "NewVRSystemHandPitch storage contract was not found")
        hand_pitch_storage, hand_pitch_default = hand_pitch
        image.require_writable_rva(hand_pitch_storage, 4)
        evidence.append(
            f"hand pitch: dword default {hand_pitch_default} degrees with name"
            f" pointer at storage+8 -> 0x{hand_pitch_storage:x}")

        selector_hits = scan_text_bytes(image, POINTER_GETTER_SELECTOR_PATTERN)
        movups_hits = scan_text_bytes(image, POINTER_GETTER_MOVUPS_PATTERN)
        getter_candidates: set[int] = set()
        for hit in selector_hits + movups_hits:
            try:
                start, _end = table.bounds(hit)
            except AnalyzerError:
                continue
            getter_candidates.add(start)
        getter_matches: list[tuple[int, tuple[Any, ...]]] = []
        for start in sorted(getter_candidates):
            instructions = disasm.function(*table.bounds(start))
            if match_pointer_getter(instructions):
                getter_matches.append((start, instructions))
        if not getter_matches:
            raise AnalyzerError(
                "no function satisfies the world-space pointer getter contract")
        if len(getter_matches) > 1:
            listed = ", ".join(f"0x{start:x}" for start, _ in getter_matches)
            raise AnalyzerError(f"ambiguous pointer getter matches: {listed}")
        pointer_getter, getter_instructions = getter_matches[0]
        workspace_getter, camera_slot = extract_pointer_getter_anchors(
            getter_instructions, image)
        evidence.append("pointer getter: UserCFrame selector"
                        f" +0x{USERCFRAME_SELECTOR_OFFSET:x},"
                        f" base +0x{USERCFRAME_BASE_OFFSET:x}, stride"
                        f" 0x{USERCFRAME_STRIDE:x},"
                        f" HeadScale +0x{CAMERA_HEAD_SCALE_OFFSET:x}"
                        f" -> 0x{pointer_getter:x}")
        evidence.append(f"workspace getter 0x{workspace_getter:x}"
                        " via mov rdi,rsi + direct call;"
                        f" camera via call [rcx+0x{camera_slot:x}]")

        return {
            "schema_version": 1,
            "tool": "find_roblox_vr_offsets",
            "elf": {
                "path": str(elf_path),
                "size": image.file_size,
                "build_id": image.build_id,
                "sha256": image.sha256,
            },
            "vr_debug_device_bridge": {
                "vtable_rva":
                    f"0x{vtable:x}",
                "constructor_rva":
                    f"0x{constructor:x}",
                "state_getter_rva":
                    f"0x{state_getter:x}",
                "eye_getter_rva":
                    f"0x{eye_getter:x}",
                "eye_initializer_rva":
                    f"0x{eye_initializer:x}",
                "emulator_flag_storage_rva":
                    f"0x{registration_storage:x}",
                "device_create_framebuffer_vtable_offset":
                    f"0x{framebuffer_slot:x}",
            },
            "vr_bridge_supplement": {
                "state_getter_vtable_slot_offset":
                    f"0x{state_slot:x}",
                "eye_getter_vtable_slot_offset":
                    f"0x{eye_slot:x}",
                "eye_initializer_vtable_slot_offset":
                    f"0x{initializer_slot:x}",
                "haptic_sink_rva":
                    f"0x{haptic_sink:x}",
                "haptic_sink_vtable_slot_offset":
                    f"0x{haptic_slot:x}",
                "world_pointer_getter_rva":
                    f"0x{pointer_getter:x}",
                "workspace_getter_rva":
                    f"0x{workspace_getter:x}",
                "workspace_camera_vtable_offset":
                    f"0x{camera_slot:x}",
                "new_vr_system_hand_pitch_storage_rva":
                    f"0x{hand_pitch_storage:x}",
                "new_vr_system_hand_pitch_default_degrees":
                    hand_pitch_default,
            },
            "evidence": evidence,
            "warnings": warnings,
        }


def write_json_atomic(path: pathlib.Path, payload: dict[str, Any]) -> None:
    encoded = json.dumps(payload, indent=2) + "\n"
    if len(encoded.encode("utf-8")) > MAX_JSON_OUTPUT_BYTES:
        raise AnalyzerError("analysis result exceeds the output size limit")
    parent = path.parent
    try:
        handle = tempfile.NamedTemporaryFile("w",
                                             encoding="utf-8",
                                             dir=parent,
                                             prefix=path.name,
                                             delete=False)
        with handle:
            handle.write(encoded)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(handle.name, path)
    except OSError as error:
        raise AnalyzerError(f"cannot write output file: {path}") from error


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="find-roblox-vr-offsets",
        description=(
            "Extract and validate the native VR bridge offsets from an x86-64"
            " Roblox libroblox.so payload."),
    )
    parser.add_argument(
        "--elf",
        required=True,
        type=pathlib.Path,
        help="path to the libroblox.so ELF image to analyze",
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=None,
        help="optional path for the JSON result (written atomically)",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="do not echo the JSON result to stdout when --output is given",
    )
    return parser.parse_args(list(argv))


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        result = find_vr_offsets(arguments.elf)
        if arguments.output is not None:
            write_json_atomic(arguments.output, result)
            if not arguments.quiet:
                print(json.dumps(result, indent=2))
        else:
            print(json.dumps(result, indent=2))
    except AnalyzerError as error:
        print(f"find-roblox-vr-offsets: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
