#!/usr/bin/env python3
"""Regenerate the checked-in freestanding runtime; never run by kru itself.

Requires GNU-compatible cc, ld, objcopy and nm only during offline regeneration.
All runtime code/data lives in .text; linking at zero resolves its internal PC
relative references before extraction. The compiler copies the resulting bytes
and uses the exported symbol offsets, so runtime compilation needs no toolchain.
"""
import os
import argparse
import hashlib
import re
import struct
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SYMBOLS = [
    'slice_validate',
    'slice_sys_write', 'slice_print', 'slice_len', 'slice_eq', 'slice_index',
    'slice_concat', 'slice_sys_open', 'slice_file_open', 'slice_file_open_write',
    'owned_file_read', 'slice_free',
    'slice_checked_read', 'slice_file_read', 'slice_args_get', 'slice_read_int', 'slice_string_push',
    'int128_to_float', 'float_to_int128',
    'read_int',
    'string_push', 'string_view', 'string_drop',
    'sys_open', 'sys_close', 'sys_write', 'checked_read',
    'print_char',
    'add128', 'sub128', 'mul128', 'and128', 'or128', 'xor128', 'udiv128', 'sdiv128',
    'umod128', 'smod128', 'shl128', 'ushr128', 'sshr128', 'cmp128', 'print_i128', 'print_u128',
    'print_f32', 'print_f64', 'print_i64', 'print_u64', 'print_i32', 'print_str', 'str_len', 'str_eq', 'str_concat', 'str_index', 'file_open',
    'file_open_write', 'file_close', 'file_write', 'file_read',
    'mem_alloc', 'mem_free', 'mem_realloc', 'args_count', 'args_get',
]

def run(*args):
    return subprocess.check_output(args, text=True)


def validate_relocations(path):
    # Only position-relative relocations survive moving the linked blob into
    # an arbitrary generated executable. Absolute pointers would target zero.
    for relocation in re.findall(r'R_X86_64_[A-Z0-9_]+', run('readelf', '-rW', str(path))):
        if relocation not in {'R_X86_64_PC32', 'R_X86_64_PLT32'}:
            raise RuntimeError(f'non-position-independent runtime relocation: {relocation}')


def validate_linked_sections(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise RuntimeError('runtime must be little-endian ELF64')
    header = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    offset, stride, count, names_index = header[5], header[10], header[11], header[12]
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * stride) for i in range(count)]
    names = sections[names_index]
    strings = data[names[4]:names[4] + names[5]]
    for section in sections:
        name = strings[section[0]:].split(b'\0', 1)[0]
        if section[5] and section[2] & 2 and name != b'.text':
            raise RuntimeError(f'allocated runtime section omitted from blob: {name!r}')
        if section[5] and section[1] in {4, 9}:
            raise RuntimeError('linked runtime still contains relocations')


def source_fingerprint():
    digest=hashlib.sha256()
    for name in ('tools/generate_native_runtime.py','tools/native_runtime.S',
                 'tools/native_numeric_runtime.c','include/utf8.h'):
        data=(ROOT/name).read_bytes()
        digest.update(name.encode()+b'\0'+str(len(data)).encode()+b'\0'+data)
    return digest.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-source',action='store_true',help='check source provenance without invoking a toolchain')
    args=parser.parse_args()
    if args.verify_source:
        header=(ROOT/'include/native_runtime_blob.h').read_text()
        if f'Runtime source SHA256: {source_fingerprint()}' not in header:
            raise SystemExit('runtime blob source fingerprint is stale; regenerate it with tools/generate_native_runtime.py')
        print('PASS native-runtime/checked-in source fingerprint')
        return
    with tempfile.TemporaryDirectory(prefix='kru-runtime-') as temporary:
        work = Path(temporary)
        obj, elf, blob = (work / name for name in ('runtime.o', 'runtime.elf', 'runtime.bin'))
        run(os.environ.get('CC', 'cc'), '-c', str(ROOT / 'tools/native_runtime.S'), '-o', str(obj))
        numeric = work / 'numeric.o'
        run(os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-ffreestanding',
            '-fno-builtin', '-fno-stack-protector', '-fno-asynchronous-unwind-tables',
            '-fPIC', '-mno-red-zone', '-ffixed-r12', '-c', str(ROOT / 'tools/native_numeric_runtime.c'), '-o', str(numeric))
        validate_relocations(obj)
        validate_relocations(numeric)
        script = work / 'runtime.ld'
        script.write_text('SECTIONS { . = 0; .text : { *(.text*) *(.rodata*) } /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) } }')
        run(os.environ.get('LD', 'ld'), '-nostdlib', '--no-undefined', '-T', str(script),
            '-e', 'rt_print_i32', str(obj), str(numeric), '-o', str(elf))
        # Reject unresolved symbols before extracting the linked runtime.
        if run('nm', '-u', str(elf)).strip():
            raise RuntimeError('runtime contains unresolved symbols')
        validate_linked_sections(elf)
        run('objcopy', '-O', 'binary', '--only-section=.text', str(elf), str(blob))
        symbols = {}
        for line in run('nm', '-n', str(elf)).splitlines():
            parts = line.split()
            if len(parts) == 3:
                symbols[parts[2]] = int(parts[0], 16)
        data = blob.read_bytes()
        offsets = [symbols['rt_' + symbol] for symbol in SYMBOLS]
        if any(offset >= len(data) for offset in offsets):
            raise RuntimeError('exported runtime symbol outside blob')
        lines = [
            '/* Generated by tools/generate_native_runtime.py; do not edit. */',
            f'/* Runtime source SHA256: {source_fingerprint()} */',
            '#ifndef KRU_NATIVE_RUNTIME_BLOB_H', '#define KRU_NATIVE_RUNTIME_BLOB_H',
            '#include <stdint.h>', '', 'enum native_runtime_symbol {',
        ]
        lines += ['    RT_' + symbol.upper() + ',' for symbol in SYMBOLS]
        lines += ['    RT_COUNT', '};', '', 'static const uint32_t native_runtime_offsets[RT_COUNT] = {']
        lines += ['    ' + str(offset) + ', /* rt_' + symbol + ' */' for symbol, offset in zip(SYMBOLS, offsets)]
        lines += ['};', '', 'static const unsigned char native_runtime_blob[] = {']
        for index in range(0, len(data), 12):
            lines.append('    ' + ', '.join(f'0x{byte:02x}' for byte in data[index:index + 12]) + ',')
        lines += ['};', '', '#endif', '']
        (ROOT / 'include/native_runtime_blob.h').write_text('\n'.join(lines))
        print(f'Generated {len(data)} runtime bytes and {len(offsets)} entry offsets.')

if __name__ == '__main__':
    main()
