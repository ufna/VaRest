#!/usr/bin/env python3
"""Bounded offline ELF observations; run with python3 -B.

Requires ELF64 little-endian x86_64 ET_EXEC/ET_DYN with ordinary section
headers. Never loads an artifact, applies relocations, invokes external tools,
or certifies code closure, provenance, loader ownership or runtime capability.
Packed RELR and extended ELF numbering are explicitly unsupported.
"""

import argparse
import hashlib
import json
import os
import stat
import struct
import sys


U64_MAX = (1 << 64) - 1
GETTER = "VaRestNativeOwnerBootstrap1"
LIMITS = {
    "file_bytes": 128 * 1024 * 1024,
    "program_headers": 128,
    "section_headers": 4096,
    "string_table_bytes": 16 * 1024 * 1024,
    "string_bytes": 4096,
    "decoded_string_bytes_total": 16 * 1024 * 1024,
    "symbols_total": 131072,
    "dynamic_entries": 4096,
    "relocations_total": 262144,
    "array_entries_total": 4096,
    "note_bytes_total": 256 * 1024,
    "note_records_total": 256,
    "note_owner_bytes": 256,
    "output_bytes": 48 * 1024 * 1024,
}
DYNAMIC_NAMES = {
    0: "NULL", 1: "NEEDED", 2: "PLTRELSZ", 3: "PLTGOT", 4: "HASH",
    5: "STRTAB", 6: "SYMTAB", 7: "RELA", 8: "RELASZ", 9: "RELAENT",
    10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI", 14: "SONAME",
    15: "RPATH", 16: "SYMBOLIC", 17: "REL", 18: "RELSZ", 19: "RELENT",
    20: "PLTREL", 21: "DEBUG", 22: "TEXTREL", 23: "JMPREL", 24: "BIND_NOW",
    25: "INIT_ARRAY", 26: "FINI_ARRAY", 27: "INIT_ARRAYSZ",
    28: "FINI_ARRAYSZ", 29: "RUNPATH", 30: "FLAGS", 32: "PREINIT_ARRAY",
    33: "PREINIT_ARRAYSZ", 34: "SYMTAB_SHNDX", 35: "RELRSZ", 36: "RELR",
    37: "RELRENT", 0x6FFFFEF5: "GNU_HASH", 0x6FFFFEF6: "TLSDESC_PLT",
    0x6FFFFEF7: "TLSDESC_GOT", 0x6FFFFEFB: "DEPAUDIT", 0x6FFFFEFC: "AUDIT",
    0x6FFFFFF0: "VERSYM", 0x6FFFFFF9: "RELACOUNT", 0x6FFFFFFA: "RELCOUNT",
    0x6FFFFFFB: "FLAGS_1", 0x6FFFFFFC: "VERDEF", 0x6FFFFFFD: "VERDEFNUM",
    0x6FFFFFFE: "VERNEED", 0x6FFFFFFF: "VERNEEDNUM",
    0x7FFFFFFD: "AUXILIARY", 0x7FFFFFFF: "FILTER",
}
REPEAT_TAGS = {1, 0x7FFFFFFD, 0x7FFFFFFF}
STRING_TAGS = {1, 14, 15, 29, 0x6FFFFEFB, 0x6FFFFEFC, 0x7FFFFFFD, 0x7FFFFFFF}
SYMBOL_TYPES = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION",
                4: "FILE", 5: "COMMON", 6: "TLS", 10: "GNU_IFUNC"}
RELOCATION_TYPES = {0: "NONE", 1: "64", 2: "PC32", 5: "COPY", 6: "GLOB_DAT",
                    7: "JUMP_SLOT", 8: "RELATIVE", 10: "32", 11: "32S",
                    16: "DTPMOD64", 17: "DTPOFF64", 18: "TPOFF64",
                    36: "TLSDESC", 37: "IRELATIVE"}
CLAIMS = [
    "Offline metadata observations only; input is never executed or loaded.",
    "No instruction/callback/getter closure or constructor safety certificate.",
    "No mapping provenance, trusted catalogue, ownership, census or capability certificate.",
    "Relocation records and array words are reported without applying relocations.",
    "Version definition/requirement, unwind, hash and instruction semantics are not decoded.",
]


class ObservationError(Exception):
    def __init__(self, code, detail):
        super().__init__(detail)
        self.code = code


def require(condition, code, detail):
    if not condition:
        raise ObservationError(code, detail)


def end64(start, size, context):
    require(0 <= start <= U64_MAX and 0 <= size <= U64_MAX - start,
            "arithmetic_overflow", context)
    return start + size


def span(start, count, stride, bound, context):
    require(0 <= count <= U64_MAX and 0 < stride <= U64_MAX,
            "arithmetic_overflow", context)
    require(count <= U64_MAX // stride, "arithmetic_overflow", context)
    stop = end64(start, count * stride, context)
    require(stop <= bound, "file_bounds", context)
    return stop


def text_bytes(value):
    return value.decode("utf-8", "backslashreplace")


class Elf:
    def __init__(self, data):
        self.data = data
        self.programs = []
        self.sections = []
        self.tables = {}
        self.dynamic = []
        self.tags = {}
        self.relocations = []
        self.string_bytes = 0
        self.version_sources = {}

    def unpack(self, fmt, offset, context):
        span(offset, 1, struct.calcsize(fmt), len(self.data), context)
        return struct.unpack_from(fmt, self.data, offset)

    def string(self, section, index):
        require(section["type"] == 3, "string_table", "string table section type")
        size = section["size"]
        require(size <= LIMITS["string_table_bytes"], "count_cap", "string table bytes")
        require(size > 0 and self.data[section["offset"]] == 0
                and self.data[section["offset"] + size - 1] == 0,
                "string_table", "string table must begin/end with NUL")
        require(0 <= index < size, "string_bounds", "string table offset")
        start = section["offset"] + index
        stop = self.data.find(b"\0", start, min(section["offset"] + size,
                                               start + LIMITS["string_bytes"] + 1))
        require(stop >= 0, "string_bounds", "unterminated/overlong string")
        self.string_bytes += stop - start
        require(self.string_bytes <= LIMITS["decoded_string_bytes_total"],
                "count_cap", "aggregate decoded string bytes")
        return text_bytes(self.data[start:stop])

    def va(self, address, size=1, file_backed=True):
        stop = end64(address, size, "virtual-address extent")
        matches = [p for p in self.programs if p["type"] == 1
                   and p["vaddr"] <= address
                   and stop <= p["vaddr"] + (p["filesz"] if file_backed else p["memsz"])]
        require(len(matches) == 1, "virtual_bounds", "unmapped/ambiguous virtual extent")
        p = matches[0]
        return p["offset"] + address - p["vaddr"], p["index"]

    def matching_section(self, address, size, types, context):
        offset, _ = self.va(address, size)
        matches = [s for s in self.sections if s["type"] in types
                   and s["addr"] == address and s["offset"] == offset and s["size"] == size]
        require(len(matches) == 1, "section_association", context)
        return matches[0]

    def headers(self):
        h = self.unpack("<16sHHIQQQIHHHHHH", 0, "ELF header")
        ident = h[0]
        require(ident[:4] == b"\x7fELF", "elf_magic", "not ELF")
        require(ident[4:7] == bytes((2, 1, 1)), "unsupported_format", "requires ELF64 LE version1")
        require(h[1] in (2, 3) and h[2] == 62 and h[3] == 1,
                "unsupported_format", "requires EM_X86_64 ET_EXEC/ET_DYN version1")
        require(h[8] == 64 and h[9] == 56 and h[11] == 64,
                "unsupported_format", "nonstandard ELF/header entry size")
        require(h[10] != 0xFFFF and h[12] != 0 and h[13] != 0xFFFF and h[6] != 0,
                "unsupported_sections", "section headers required; extended numbering unsupported")
        require(0 < h[10] <= LIMITS["program_headers"]
                and 0 < h[12] <= LIMITS["section_headers"], "count_cap", "header count")
        require(h[13] < h[12], "section_index", "section name table index")
        require(h[5] >= 64 and h[6] >= 64, "file_bounds", "header-table offset")
        span(h[5], h[10], 56, len(self.data), "program header table")
        span(h[6], h[12], 64, len(self.data), "section header table")
        for i in range(h[10]):
            p = self.unpack("<IIQQQQQQ", h[5] + i * 56, "program header")
            record = dict(zip(("type", "flags", "offset", "vaddr", "paddr",
                               "filesz", "memsz", "align"), p))
            record["index"] = i
            span(p[2], p[5], 1, len(self.data), "program file extent")
            end64(p[3], p[6], "program memory extent")
            require(p[7] in (0, 1) or p[7] & (p[7] - 1) == 0,
                    "alignment", "program alignment is not a power of two")
            if p[0] in (1, 7):
                require(p[5] <= p[6], "segment_size", "file size exceeds memory size")
                require(p[7] <= 1 or p[2] % p[7] == p[3] % p[7],
                        "alignment", "load/TLS file-address congruence")
            self.programs.append(record)
        require(any(p["type"] == 1 for p in self.programs), "virtual_bounds", "no PT_LOAD")
        loads = sorted((p["vaddr"], p["vaddr"] + p["memsz"])
                       for p in self.programs if p["type"] == 1 and p["memsz"])
        require(all(a[1] <= b[0] for a, b in zip(loads, loads[1:])),
                "virtual_bounds", "overlapping PT_LOAD memory extents unsupported")
        for i in range(h[12]):
            s = self.unpack("<IIQQQQIIQQ", h[6] + i * 64, "section header")
            record = dict(zip(("name_offset", "type", "flags", "addr", "offset", "size",
                               "link", "info", "align", "entsize"), s))
            record["index"] = i
            end64(s[3], s[5], "section virtual extent")
            if s[1] not in (0, 8):
                span(s[4], s[5], 1, len(self.data), "section file extent")
            require(s[8] in (0, 1) or s[8] & (s[8] - 1) == 0,
                    "alignment", "section alignment is not a power of two")
            require(s[1] != 19, "unsupported_relr", "packed SHT_RELR is unsupported")
            self.sections.append(record)
        require(self.sections[0]["type"] == 0, "section_index", "section zero is not NULL")
        names = self.sections[h[13]]
        for s in self.sections:
            s["name"] = self.string(names, s["name_offset"])
            if s["type"] not in (0, 8) and s["flags"] & 2 and s["size"]:
                offset, _ = self.va(s["addr"], s["size"])
                require(offset == s["offset"], "section_association", "allocated section file/VA mismatch")
        return {"type": h[1], "machine": h[2], "entry": h[4], "flags": h[7],
                "osabi": ident[7], "abi_version": ident[8], "phoff": h[5],
                "shoff": h[6], "phnum": h[10], "shnum": h[12], "shstrndx": h[13]}

    def symbols(self):
        total = 0
        for s in self.sections:
            if s["type"] not in (2, 11):
                continue
            require(s["entsize"] == 24 and s["size"] % 24 == 0,
                    "entry_size", "symbol entry size")
            count = s["size"] // 24
            total += count
            require(total <= LIMITS["symbols_total"], "count_cap", "symbol count")
            require(s["link"] < len(self.sections), "section_index", "symbol string link")
            strings = self.sections[s["link"]]
            records = []
            for i in range(count):
                name, info, other, shndx, value, size = self.unpack(
                    "<IBBHQQ", s["offset"] + i * 24, "symbol")
                end64(value, size, "symbol extent")
                require(i != 0 or (name, info, other, shndx, value, size) == (0, 0, 0, 0, 0, 0),
                        "symbol_zero", "symbol zero must be the reserved null entry")
                require(shndx != 0xFFFF, "unsupported_sections", "extended symbol section index")
                require(shndx < len(self.sections) or shndx >= 0xFF00,
                        "section_index", "symbol section index")
                records.append({"index": i, "name": self.string(strings, name),
                                "binding": info >> 4, "type": info & 15,
                                "type_name": SYMBOL_TYPES.get(info & 15, "UNKNOWN"),
                                "visibility": other & 3, "other": other,
                                "shndx": shndx, "value": value, "size": size})
            self.tables[s["index"]] = {"section": s["index"], "name": s["name"],
                                       "type": s["type"], "symbols": records}
        for s in self.sections:
            if s["type"] != 0x6FFFFFFF:
                continue
            require(s["link"] in self.tables and self.tables[s["link"]]["type"] == 11,
                    "section_index", "version-symbol table link")
            records = self.tables[s["link"]]["symbols"]
            require(s["size"] == 2 * len(records), "entry_size", "version-symbol table size")
            require(s["link"] not in self.version_sources, "ambiguous_versym",
                    "multiple version-symbol sources for one dynamic symbol table")
            self.version_sources[s["link"]] = s

    def annotate_versions(self):
        # Only the unique actual DT_VERSYM source may annotate getter input.
        for link, s in self.version_sources.items():
            require(s["flags"] & 2 and self.tags.get(0x6FFFFFF0) == s["addr"]
                    and self.tags.get(6) == self.sections[link]["addr"]
                    and self.va(s["addr"], s["size"])[0] == s["offset"],
                    "section_association", "version-symbol source is not the actual mapped DT_VERSYM table")
            records = self.tables[link]["symbols"]
            for i, symbol in enumerate(records):
                symbol["version_index_raw"] = self.unpack("<H", s["offset"] + i * 2, "versym")[0]

    def getter(self):
        definitions = []
        sources = []
        undefined = []
        for table in self.tables.values():
            matches = [s for s in table["symbols"] if s["name"] == GETTER]
            require(len(matches) <= 1, "ambiguous_getter", "duplicate getter name within symbol table")
            for s in matches:
                require(s["type"] != 10, "getter_ifunc", "permanent getter is GNU_IFUNC")
                if s["shndx"] == 0:
                    undefined.append(table["section"])
                    continue
                require(s["type"] == 2 and s["binding"] == 1 and s["visibility"] in (0, 3)
                        and s["other"] == s["visibility"], "getter_kind", "requires ordinary global STT_FUNC getter")
                require(0 < s["shndx"] < len(self.sections) and s["size"] > 0,
                        "getter_extent", "getter section/size is not ordinary defined extent")
                section = self.sections[s["shndx"]]
                require(section["type"] == 1 and section["flags"] & 6 == 6
                        and section["addr"] <= s["value"]
                        and s["value"] + s["size"] <= section["addr"] + section["size"],
                        "getter_extent", "getter outside allocated executable PROGBITS section")
                section_offset, _ = self.va(section["addr"], section["size"])
                require(section_offset == section["offset"], "section_association",
                        "getter defining section file/VA mismatch")
                offset, load = self.va(s["value"], s["size"])
                require(offset == section["offset"] + s["value"] - section["addr"],
                        "section_association", "getter function file/VA mismatch")
                require(self.programs[load]["flags"] & 1, "getter_extent", "getter load is not executable")
                require(s.get("version_index_raw", 1) in (0, 1),
                        "unsupported_getter_version", "versioned permanent getter unsupported")
                definition = {k: s[k] for k in ("value", "size", "shndx", "binding", "type", "visibility")}
                definition.update(file_offset=offset, load_segment=load,
                                  raw_function_extent_sha256=hashlib.sha256(self.data[offset:offset + s["size"]]).hexdigest())
                definitions.append(definition)
                sources.append(table["section"])
        unique = {json.dumps(d, sort_keys=True) for d in definitions}
        require(len(unique) <= 1 and not (definitions and undefined),
                "ambiguous_getter", "getter definitions/origins disagree")
        return {"name": GETTER, "definition": definitions[0] if definitions else None,
                "definition_tables": sources, "undefined_tables": undefined}

    def dynamic_table(self, elf_type):
        programs = [p for p in self.programs if p["type"] == 2]
        sections = [s for s in self.sections if s["type"] == 6]
        require(len(programs) <= 1 and len(sections) <= 1,
                "dynamic_layout", "multiple dynamic tables unsupported")
        if not programs:
            require(not sections and elf_type == 2, "dynamic_layout", "missing PT_DYNAMIC")
            return
        require(len(sections) == 1, "unsupported_sections", "dynamic section required")
        p, s = programs[0], sections[0]
        require(p["offset"] == s["offset"] and p["vaddr"] == s["addr"]
                and p["filesz"] == s["size"] and s["entsize"] == 16 and s["size"] % 16 == 0,
                "dynamic_layout", "PT_DYNAMIC/section mismatch")
        mapped_offset, _ = self.va(p["vaddr"], p["filesz"])
        require(mapped_offset == p["offset"], "section_association",
                "complete PT_DYNAMIC/SHT_DYNAMIC table file/VA mismatch")
        require(s["flags"] & 2 and 0 < p["filesz"] == p["memsz"],
                "dynamic_layout", "requires an allocated entirely file-backed dynamic table")
        require(s["size"] // 16 <= LIMITS["dynamic_entries"], "count_cap", "dynamic count")
        require(s["link"] < len(self.sections), "section_index", "dynamic string link")
        strings = self.sections[s["link"]]
        terminated = False
        for i in range(s["size"] // 16):
            tag, value = self.unpack("<qQ", s["offset"] + i * 16, "dynamic entry")
            if terminated:
                require(tag == 0 and value == 0, "dynamic_termination", "nonzero data after DT_NULL")
                continue
            if tag == 0:
                require(value == 0, "dynamic_termination", "nonzero DT_NULL value")
                terminated = True
                continue
            if tag in DYNAMIC_NAMES and tag not in REPEAT_TAGS:
                require(tag not in self.tags, "duplicate_dynamic_tag", DYNAMIC_NAMES[tag])
            self.tags[tag] = value
            record = {"index": i, "tag": tag, "tag_name": DYNAMIC_NAMES.get(tag, "UNKNOWN"), "value": value}
            if tag in STRING_TAGS:
                record["string"] = self.string(strings, value)
            self.dynamic.append(record)
        require(terminated, "dynamic_termination", "missing DT_NULL")
        require(not any(t in self.tags for t in (35, 36, 37)),
                "unsupported_relr", "packed DT_RELR is unsupported")
        require(34 not in self.tags, "unsupported_sections", "extended dynamic symbol indexes unsupported")
        require(5 in self.tags and 10 in self.tags, "dynamic_layout", "missing dynamic string address/size")
        require(strings["type"] == 3 and self.tags[5] == strings["addr"]
                and self.tags[10] == strings["size"]
                and self.va(self.tags[5], self.tags[10])[0] == strings["offset"],
                "section_association", "dynamic string file/VA association")
        if 6 in self.tags or 11 in self.tags:
            require(6 in self.tags and self.tags.get(11) == 24,
                    "entry_size", "DT_SYMTAB/DT_SYMENT")
            matches = [s for s in self.sections if s["type"] == 11 and s["addr"] == self.tags[6]]
            require(len(matches) == 1 and matches[0]["link"] == strings["index"]
                    and self.va(matches[0]["addr"], matches[0]["size"])[0] == matches[0]["offset"],
                    "section_association", "dynamic symbol file/VA association")
        if 0x6FFFFFF0 in self.tags:
            matches = [s for s in self.sections if s["type"] == 0x6FFFFFFF
                       and s["addr"] == self.tags[0x6FFFFFF0]]
            require(len(matches) == 1 and self.va(matches[0]["addr"], matches[0]["size"])[0] == matches[0]["offset"]
                    and self.sections[matches[0]["link"]]["addr"] == self.tags.get(6),
                    "section_association", "dynamic versym file/VA association")
        for address_tag, size_tag, entry_tag, kind, width in ((7, 8, 9, 4, 24), (17, 18, 19, 9, 16)):
            if address_tag in self.tags or size_tag in self.tags:
                require(all(t in self.tags for t in (address_tag, size_tag, entry_tag))
                        and self.tags[entry_tag] == width, "dynamic_layout", "dynamic relocation triple")
                self.matching_section(self.tags[address_tag], self.tags[size_tag], (kind,), "dynamic relocation section")
        for count_tag, size_tag, width in ((0x6FFFFFF9, 8, 24), (0x6FFFFFFA, 18, 16)):
            if count_tag in self.tags:
                require(size_tag in self.tags and self.tags[count_tag] <= self.tags[size_tag] // width,
                        "dynamic_layout", "relative relocation count exceeds table")
        if any(t in self.tags for t in (2, 20, 23)):
            require(all(t in self.tags for t in (2, 20, 23)) and self.tags[20] in (7, 17),
                    "dynamic_layout", "PLT relocation triple")
            self.matching_section(self.tags[23], self.tags[2], (4 if self.tags[20] == 7 else 9,), "PLT relocation section")

    def relocation_tables(self):
        count = 0
        for s in self.sections:
            if s["type"] not in (4, 9):
                continue
            width = 24 if s["type"] == 4 else 16
            require(s["entsize"] == width and s["size"] % width == 0,
                    "entry_size", "relocation entry size")
            count += s["size"] // width
            require(count <= LIMITS["relocations_total"], "count_cap", "relocation count")
            require(s["link"] in self.tables, "section_index", "relocation symbol link")
            require(s["info"] < len(self.sections), "section_index", "relocation target section")
            symbols = self.tables[s["link"]]["symbols"]
            records = []
            for i in range(s["size"] // width):
                fields = self.unpack("<QQq" if width == 24 else "<QQ", s["offset"] + i * width, "relocation")
                address, info = fields[:2]
                symbol, kind = info >> 32, info & 0xFFFFFFFF
                require(symbol < len(symbols), "symbol_index", "relocation symbol index")
                record = {"index": i, "offset_va": address, "symbol": symbol, "type": kind,
                          "type_name": "R_X86_64_" + RELOCATION_TYPES.get(kind, "UNKNOWN")}
                if width == 24:
                    record["addend"] = fields[2]
                if kind != 0 and s["flags"] & 2:
                    _, record["target_load_segment"] = self.va(address, 1, False)
                    try:
                        record["target_file_offset"] = self.va(address)[0]
                    except ObservationError:
                        record["target_file_offset"] = None
                records.append(record)
            self.relocations.append({"section": s["index"], "name": s["name"],
                                     "symbol_table": s["link"], "relocations": records})

    def arrays(self):
        arrays = []
        count = 0
        for s in self.sections:
            if s["type"] not in (14, 15, 16) and s["name"] not in (".ctors", ".dtors"):
                continue
            require(s["size"] % 8 == 0 and s["entsize"] in (0, 8), "entry_size", "init/fini array")
            count += s["size"] // 8
            require(count <= LIMITS["array_entries_total"], "count_cap", "init/fini array entries")
            words = [self.unpack("<Q", s["offset"] + i * 8, "array word")[0] for i in range(s["size"] // 8)]
            arrays.append({"section": s["index"], "name": s["name"], "addr": s["addr"],
                           "file_offset": s["offset"], "raw_words": words})
        for a, z, kind in ((25, 27, 14), (26, 28, 15), (32, 33, 16)):
            if a in self.tags or z in self.tags:
                require(a in self.tags and z in self.tags, "dynamic_layout", "init/fini array pair")
                self.matching_section(self.tags[a], self.tags[z], (kind,), "dynamic init/fini array association")
        functions = []
        for tag in (12, 13):
            if self.tags.get(tag):
                offset, load = self.va(self.tags[tag])
                require(self.programs[load]["flags"] & 1, "virtual_bounds", "INIT/FINI is not executable")
                functions.append({"tag_name": DYNAMIC_NAMES[tag], "addr": self.tags[tag], "file_offset": offset})
        return arrays, functions

    def notes(self):
        sources = [("section:" + str(s["index"]), s["offset"], s["size"])
                   for s in self.sections if s["type"] == 7]
        sources += [("segment:" + str(p["index"]), p["offset"], p["filesz"])
                    for p in self.programs if p["type"] == 4]
        require(sum(s[2] for s in sources) <= LIMITS["note_bytes_total"], "count_cap", "raw note bytes")
        notes = {}
        for source, offset, size in sources:
            stop = span(offset, size, 1, len(self.data), "note extent")
            while offset < stop:
                require(stop - offset >= 12, "note_bounds", "truncated note header/padding")
                namesz, descsz, kind = self.unpack("<III", offset, "note header")
                require(namesz <= LIMITS["note_owner_bytes"], "count_cap", "note owner bytes")
                name_start = offset + 12
                desc_start = end64(name_start, (namesz + 3) & ~3, "note owner extent")
                next_offset = end64(desc_start, (descsz + 3) & ~3, "note descriptor extent")
                require(next_offset <= stop, "note_bounds", "truncated note payload")
                name = self.data[name_start:name_start + namesz]
                desc = self.data[desc_start:desc_start + descsz]
                key = (offset, next_offset)
                if key not in notes:
                    require(len(notes) < LIMITS["note_records_total"], "count_cap", "note records")
                    notes[key] = {"file_offset": offset, "type": kind,
                                  "owner_hex": name.hex(), "descriptor_hex": desc.hex(), "sources": []}
                    if name == b"GNU\0" and kind == 3:
                        require(0 < descsz <= 64, "build_id", "GNU build ID length")
                        notes[key]["gnu_build_id"] = desc.hex()
                notes[key]["sources"].append(source)
                offset = next_offset
        ids = sorted({n["gnu_build_id"] for n in notes.values() if "gnu_build_id" in n})
        require(len(ids) <= 1, "build_id", "conflicting GNU build IDs")
        return list(notes.values()), ids[0] if ids else None

    def observe(self):
        require(len(self.data) <= LIMITS["file_bytes"], "count_cap", "file bytes")
        header = self.headers()
        self.symbols()
        self.dynamic_table(header["type"])
        self.annotate_versions()
        getter = self.getter()
        self.relocation_tables()
        arrays, functions = self.arrays()
        notes, build_id = self.notes()
        return {"header": header, "program_headers": self.programs,
                "sections": self.sections, "symbol_tables": list(self.tables.values()),
                "getter": getter, "dynamic": self.dynamic,
                "needed": [d["string"] for d in self.dynamic if d["tag"] == 1],
                "textrel_tag": 22 in self.tags, "df_textrel": bool(self.tags.get(30, 0) & 4),
                "relocation_tables": self.relocations, "init_fini_arrays": arrays,
                "init_fini_functions": functions,
                "tls_segments": [p for p in self.programs if p["type"] == 7],
                "notes": notes, "gnu_build_id": build_id}


def identity(s):
    return {k: getattr(s, "st_" + k) for k in ("dev", "ino", "size", "mtime_ns", "ctime_ns", "mode")}


def read_fd(fd):
    chunks = []
    total = 0
    while True:
        chunk = os.read(fd, min(1024 * 1024, LIMITS["file_bytes"] + 1 - total))
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)
        total += len(chunk)
        require(total <= LIMITS["file_bytes"], "count_cap", "file grew beyond byte limit")


def observe_file(path):
    path = os.path.abspath(path)
    require(hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_CLOEXEC"),
            "unsupported_io", "O_NOFOLLOW/O_CLOEXEC required")
    before_path = os.stat(path, follow_symlinks=False)
    require(stat.S_ISREG(before_path.st_mode), "input_type", "input must be regular and not symlink")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and identity(before) == identity(before_path),
                "input_changed", "open/path identity mismatch")
        require(0 < before.st_size <= LIMITS["file_bytes"], "count_cap", "file size")
        data = read_fd(fd)
        require(len(data) == before.st_size, "input_changed", "short/changed input")
        first_hash = hashlib.sha256(data).hexdigest()
        result = {"path": path, "before": identity(before), "sha256": first_hash}
        try:
            result["elf"] = Elf(data).observe()
            result["status"] = "observed"
        except ObservationError as exc:
            result.update(status="unsupported_or_malformed", error={"code": exc.code, "detail": str(exc)})
        os.lseek(fd, 0, os.SEEK_SET)
        second_hash = hashlib.sha256(read_fd(fd)).hexdigest()
        after, after_path = os.fstat(fd), os.stat(path, follow_symlinks=False)
        require(identity(before) == identity(after) == identity(after_path)
                and first_hash == second_hash, "input_changed", "input identity/bytes changed during observation")
        result.update(after=identity(after), after_path=identity(after_path), sha256_after=second_hash)
        return result
    finally:
        os.close(fd)


def fixture(duplicate=False):
    """Tiny synthetic ELF; metadata fixture only, never written or executed."""
    names = b"\0.text\0.dynstr\0.symtab\0.dynamic\0.shstrtab\0"
    strings = b"\0" + GETTER.encode("ascii") + b"\0libCore.so\0"
    offsets = {"text": 256, "strings": 272}
    offsets["symbols"] = (272 + len(strings) + 7) & ~7
    nsym = 3 if duplicate else 2
    offsets["dynamic"] = offsets["symbols"] + nsym * 24
    offsets["names"] = offsets["dynamic"] + 5 * 16
    offsets["sections"] = (offsets["names"] + len(names) + 7) & ~7
    data = bytearray(offsets["sections"] + 6 * 64)
    base = 0x400000
    struct.pack_into("<16sHHIQQQIHHHHHH", data, 0, b"\x7fELF\x02\x01\x01" + bytes(9),
                     3, 62, 1, 0, 64, offsets["sections"], 0, 64, 56, 2, 64, 6, 5)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, base, base, len(data), len(data), 4096)
    struct.pack_into("<IIQQQQQQ", data, 120, 2, 4, offsets["dynamic"], base + offsets["dynamic"],
                     base + offsets["dynamic"], 80, 80, 8)
    data[256:272] = b"\xc3" + bytes(15)
    data[272:272 + len(strings)] = strings
    data[offsets["names"]:offsets["names"] + len(names)] = names
    for index in range(1, nsym):
        struct.pack_into("<IBBHQQ", data, offsets["symbols"] + index * 24,
                         1, 0x12, 0, 1, base + 256, 1)
    dynamic = [(5, base + 272), (10, len(strings)), (1, strings.index(b"libCore.so")), (0, 0), (0, 0)]
    for index, pair in enumerate(dynamic):
        struct.pack_into("<qQ", data, offsets["dynamic"] + index * 16, *pair)
    specs = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
             (names.index(b".text"), 1, 6, base + 256, 256, 16, 0, 0, 16, 0),
             (names.index(b".dynstr"), 3, 2, base + 272, 272, len(strings), 0, 0, 1, 0),
             (names.index(b".symtab"), 2, 0, 0, offsets["symbols"], nsym * 24, 2, 1, 8, 24),
             (names.index(b".dynamic"), 6, 2, base + offsets["dynamic"], offsets["dynamic"], 80, 2, 0, 8, 16),
             (names.index(b".shstrtab"), 3, 0, 0, offsets["names"], len(names), 0, 0, 1, 0)]
    for index, fields in enumerate(specs):
        struct.pack_into("<IIQQQQIIQQ", data, offsets["sections"] + index * 64, *fields)
    return data, offsets


def version_source_fixture():
    """Review A's actual-version2/later-unrelated-version1 metadata witness."""
    original, offsets = fixture()
    actual = len(original)
    unrelated = actual + 4
    dynamic = actual + 8
    sections = dynamic + 96
    data = original + bytearray(8 + 96 + 8 * 64)
    data[sections:sections + 6 * 64] = original[offsets["sections"]:]
    struct.pack_into("<Q", data, 40, sections)
    struct.pack_into("<H", data, 60, 8)
    struct.pack_into("<QQ", data, 64 + 32, len(data), len(data))
    struct.pack_into("<QQQQQ", data, 120 + 8, dynamic, 0x400000 + dynamic,
                     0x400000 + dynamic, 96, 96)
    struct.pack_into("<I", data, sections + 3 * 64 + 4, 11)
    struct.pack_into("<QQ", data, sections + 3 * 64 + 8, 2, 0x400000 + offsets["symbols"])
    struct.pack_into("<QQQ", data, sections + 4 * 64 + 16,
                     0x400000 + dynamic, dynamic, 96)
    struct.pack_into("<HHHH", data, actual, 0, 2, 0, 1)
    tags = [(5, 0x400000 + offsets["strings"]), (10, 40),
            (6, 0x400000 + offsets["symbols"]), (11, 24),
            (0x6FFFFFF0, 0x400000 + actual), (0, 0)]
    for i, pair in enumerate(tags):
        struct.pack_into("<qQ", data, dynamic + i * 16, *pair)
    struct.pack_into("<IIQQQQIIQQ", data, sections + 6 * 64,
                     0, 0x6FFFFFFF, 2, 0x400000 + actual, actual, 4, 3, 0, 2, 2)
    struct.pack_into("<IIQQQQIIQQ", data, sections + 7 * 64,
                     0, 0x6FFFFFFF, 0, 0, unrelated, 4, 3, 0, 2, 2)
    return data, sections


def selftest():
    cases = []
    original, o = fixture()
    positive = Elf(bytes(original)).observe()
    require(positive["getter"]["definition"]["size"] == 1
            and positive["needed"] == ["libCore.so"], "selftest", "positive fixture observations")
    cases.append({"case": "ordinary_defined_getter_and_needed", "result": "pass"})
    def reject(name, expected, mutate=None, data=None):
        altered = bytearray(original if data is None else data)
        if mutate:
            mutate(altered)
        try:
            Elf(bytes(altered)).observe()
        except ObservationError as exc:
            require(exc.code == expected, "selftest", name + ": unexpected " + exc.code)
            cases.append({"case": name, "result": "pass", "observed_rejection": exc.code})
            return
        raise ObservationError("selftest", name + ": malformed input accepted")
    def put(fmt, offset, value):
        return lambda b: struct.pack_into(fmt, b, offset, value)
    reject("short_header", "file_bounds", data=original[:30])
    reject("short_section_table", "file_bounds", data=original[:-1])
    reject("wrong_machine", "unsupported_format", put("<H", 18, 183))
    reject("extended_sections", "unsupported_sections", put("<H", 60, 0))
    reject("section_count_cap", "count_cap", put("<H", 60, LIMITS["section_headers"] + 1))
    reject("load_address_overflow", "arithmetic_overflow", put("<Q", 64 + 16, U64_MAX - 3))
    reject("load_file_exceeds_memory", "segment_size", put("<Q", 64 + 40, 1))
    reject("section_file_overflow", "arithmetic_overflow", put("<Q", o["sections"] + 64 + 24, U64_MAX - 1))
    reject("bad_symbol_string_index", "string_bounds", put("<I", o["symbols"] + 24, 0xFFFFFFFF))
    reject("getter_ifunc", "getter_ifunc", put("<B", o["symbols"] + 24 + 4, 0x1A))
    reject("getter_zero_extent", "getter_extent", put("<Q", o["symbols"] + 24 + 16, 0))
    reject("getter_extent_overflow", "arithmetic_overflow", put("<Q", o["symbols"] + 24 + 8, U64_MAX))
    reject("ambiguous_getter", "ambiguous_getter", data=fixture(duplicate=True)[0])
    reject("duplicate_critical_dynamic_tag", "duplicate_dynamic_tag", put("<q", o["dynamic"] + 32, 10))
    reject("data_after_dynamic_null", "dynamic_termination", put("<q", o["dynamic"] + 64, 99))
    reject("dynamic_string_mapping_mismatch", "section_association", put("<Q", o["dynamic"] + 8, 0x400111))
    reject("symbol_table_entry_size", "entry_size", put("<Q", o["sections"] + 3 * 64 + 56, 16))
    reject("unterminated_section_name", "string_table",
           put("<B", o["names"] + positive["sections"][5]["size"] - 1, 65))
    reject("nonzero_reserved_symbol", "symbol_zero", put("<I", o["symbols"], 1))
    def bad_note(b):
        struct.pack_into("<I", b, o["sections"] + 3 * 64 + 4, 7)
        struct.pack_into("<III", b, o["symbols"], 4, 4096, 3)
    reject("note_payload_outside_section", "note_bounds", bad_note)
    def bad_note_owner(b):
        bad_note(b)
        struct.pack_into("<I", b, o["symbols"], LIMITS["note_owner_bytes"] + 1)
    reject("note_owner_count_cap", "count_cap", bad_note_owner)
    def bad_relocation(b):
        struct.pack_into("<I", b, o["sections"] + 4 * 64 + 4, 4)
        struct.pack_into("<Q", b, o["sections"] + 4 * 64 + 32, 24)
        struct.pack_into("<Q", b, o["sections"] + 4 * 64 + 56, 24)
        struct.pack_into("<I", b, o["sections"] + 4 * 64 + 40, 3)
        struct.pack_into("<I", b, 120, 0)
        struct.pack_into("<H", b, 16, 2)
        struct.pack_into("<QQq", b, o["dynamic"], 0x400100, 999 << 32 | 8, 0)
    reject("relocation_symbol_outside_table", "symbol_index", bad_relocation)
    bad_getter = bytearray(original)
    struct.pack_into("<Q", bad_getter, o["sections"] + 64 + 8, 4)
    struct.pack_into("<Q", bad_getter, o["sections"] + 64 + 24, 272)
    bad_dynamic = bytearray(original)
    for offset in (120 + 16, 120 + 24, o["sections"] + 4 * 64 + 16):
        struct.pack_into("<Q", bad_dynamic, offset, 0x900000)
    struct.pack_into("<Q", bad_dynamic, o["sections"] + 4 * 64 + 8, 0)
    bad_version, version_sections = version_source_fixture()
    version_control = bytearray(bad_version)
    struct.pack_into("<I", version_control, version_sections + 7 * 64 + 4, 1)
    for name, expected, data, digest in (
        ("getter_nonalloc_section_file_mapping_mismatch", "getter_extent", bad_getter,
         "b89f94a1766d1057302451326deab0264e82699f9cd37225f7db36517cdedb87"),
        ("dynamic_nonalloc_outside_all_loads", "virtual_bounds", bad_dynamic,
         "cabe5b4d569a6075ce975b1cba4e07bc3f47a140502cb45faf9870bdc7cc2e9e"),
        ("unrelated_versym_overwrites_dt_versym_version", "ambiguous_versym", bad_version,
         "537faa3c33859c27215fb41efdf6302d351e107be67d7e13df00a772cae753a1"),
        ("actual_dt_versym_version2_control", "unsupported_getter_version", version_control,
         "2a8b6b25b154fdb5a1d48ad4ed634c8a647b0550128a0a99bcc48a36e53d4ae6"),
    ):
        require(hashlib.sha256(data).hexdigest() == digest, "selftest", name + ": witness bytes drifted")
        reject(name, expected, data=data)
        cases[-1]["input_sha256"] = digest
    return {"status": "selftests_passed", "cases": cases,
            "scope": "Synthetic in-memory metadata cases; no ELF is executed or loaded."}


def exclusive_json(path, result):
    chunks = []
    size = 1
    for chunk in json.JSONEncoder(sort_keys=True, indent=2, ensure_ascii=True).iterencode(result):
        encoded_chunk = chunk.encode("utf-8")
        size += len(encoded_chunk)
        require(size <= LIMITS["output_bytes"], "count_cap", "JSON output bytes")
        chunks.append(encoded_chunk)
    encoded = b"".join(chunks) + b"\n"
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        view = memoryview(encoded)
        while view:
            written = os.write(fd, view)
            require(written > 0, "output_io", "short output write")
            view = view[written:]
        os.fsync(fd)
    finally:
        os.close(fd)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    observe = sub.add_parser("observe", help="observe one stable regular ELF file")
    observe.add_argument("artifact")
    observe.add_argument("--output", required=True, help="new exclusive JSON output path")
    tests = sub.add_parser("selftest", help="run bounded in-memory malformed ELF cases")
    tests.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    result = {"schema": 1, "observer": "VaRest offline ELF64LE x86_64 metadata observer",
              "claims_and_limits": CLAIMS, "limits": LIMITS}
    try:
        result.update(selftest() if args.command == "selftest" else observe_file(args.artifact))
    except (ObservationError, OSError) as exc:
        result.update(status="unsupported_or_malformed", error={"code": getattr(exc, "code", "io_error"), "detail": str(exc)})
    try:
        exclusive_json(args.output, result)
    except (ObservationError, OSError) as exc:
        print("output refused: " + str(exc), file=sys.stderr)
        return 2
    print(json.dumps({"status": result["status"], "output": os.path.abspath(args.output)}))
    return 0 if result["status"] in ("observed", "selftests_passed") else 2


if __name__ == "__main__":
    sys.exit(main())
