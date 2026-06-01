#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import re, sys
from pathlib import Path

for p in map(Path, sys.argv[1:]):
    text = p.read_text(errors='replace')
    print(f'=== {p.name} ===')
    print('done=', 'RVV_MEMCPY_K3_RUN_DONE' in text)
    for line in text.splitlines():
        if 'Linux version' in line or 'riscv: base ISA extensions' in line or 'riscv: ELF capabilities' in line:
            print(line)
    for line in text.splitlines():
        if 'pass:' in line and '# ' in line:
            print(line)
    for line in text.splitlines():
        if 'bench size=' in line or ' ns=' in line and any(k in line for k in ['mix ', 'scatterlist ', 'zstd_', 'xattr_style ', 'sort_records ']):
            print(line)
    current = None
    blocks = {}
    for line in text.splitlines():
        m = re.match(r'--- rvv-profile after-(\S+) ---', line)
        if m:
            current = m.group(1)
            blocks[current] = []
            continue
        if current and (line.startswith('total ') or line.startswith('reason ') or line.startswith('bucket ') or line.startswith('align ') or line.startswith('nonoverlap ') or line.startswith('overlap ')):
            blocks[current].append(line)
        elif current and line.startswith('--- '):
            current = None
    for name, lines in blocks.items():
        print(f'PROFILE {name}')
        for line in lines:
            if (line.startswith('total ') or line.startswith('reason vector ') or
                line.startswith('reason overlap ') or line.startswith('reason disallowed ') or
                line.startswith('reason small ') or line.startswith('reason disabled ') or
                line.startswith('bucket 2048') or line.startswith('bucket 4096') or
                line.startswith('bucket 8192') or line.startswith('bucket 16384') or
                line.startswith('bucket >=65536') or line.startswith('nonoverlap ') or line.startswith('overlap ')):
                print(line)
