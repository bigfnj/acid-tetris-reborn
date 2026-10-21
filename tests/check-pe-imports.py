#!/usr/bin/env python3
"""Fail if a Windows binary depends on a Visual C++ runtime DLL that a clean Windows
install does not have (MSVCP140.dll, VCRUNTIME140.dll and friends).

The Universal CRT (api-ms-win-crt-*, ucrtbase.dll) ships with Windows 10 and later,
so it is allowed. Standard library only.

Usage: python tests/check-pe-imports.py saba-reborn.exe SDL3.dll ...
"""
import re
import struct
import sys

REDIST = re.compile(r"^(msvcp|vcruntime|msvcr|concrt|vcomp|vccorlib)\d.*\.dll$", re.IGNORECASE)


def imported_dlls(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE file")
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    sections = []
    for index in range(section_count):
        _, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", data, optional + optional_size + 40 * index)
        sections.append((va, max(vsize, rsize), raw))

    def offset(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"{path}: RVA {rva:#x} is outside every section")

    def c_string(rva):
        start = offset(rva)
        return data[start:data.index(b"\0", start)].decode("ascii", "replace")

    names = []
    import_rva = struct.unpack_from("<I", data, directories + 8 * 1)[0]
    if import_rva:
        entry = offset(import_rva)
        while True:
            name_rva = struct.unpack_from("<I", data, entry + 12)[0]
            if name_rva == 0:
                break
            names.append(c_string(name_rva))
            entry += 20
    delay_rva = struct.unpack_from("<I", data, directories + 8 * 13)[0]
    if delay_rva:
        entry = offset(delay_rva)
        while True:
            name_rva = struct.unpack_from("<I", data, entry + 4)[0]
            if name_rva == 0:
                break
            names.append(c_string(name_rva) + " (delay-load)")
            entry += 32
    return names


def main(paths):
    if not paths:
        print(__doc__)
        return 2
    failures = 0
    for path in paths:
        names = imported_dlls(path)
        bad = [name for name in names if REDIST.match(name.split(" ")[0])]
        print(f"{path}: imports {', '.join(names)}")
        if bad:
            print(f"  FAIL: needs the Visual C++ redistributable: {', '.join(bad)}")
            failures += 1
        else:
            print("  PASS: no Visual C++ redistributable DLL needed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
