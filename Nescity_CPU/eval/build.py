#!/usr/bin/env python3
"""Build original, last committed optimization, and current ROM with identical flags."""
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = ROOT / 'Nescity_CPU/8bitworkshop'
CC65 = HERE / '.tools/cc65'
NESLIB = HERE / '.tools/8bitworkshop/src/worker/lib/nes'
BUILD = HERE / 'build'
FLAGS = ['-t', 'nes', '-Oirs', '-Cl', '-g', '-W', '-pointer-sign']
PREVIOUS_REF = '07b8126'
VARIANTS = ('baseline', 'previous', 'optimized')


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def tool(name, *args):
    run([CC65 / 'bin' / name, *args])


def sources(variant):
    refs = {'baseline': 'baseline', 'previous': PREVIOUS_REF}
    dest = BUILD / variant
    dest.mkdir(parents=True, exist_ok=True)
    for original in SRC.iterdir():
        if original.suffix not in ('.c', '.s', '.h'):
            continue
        data = (subprocess.check_output(['git', 'show', refs[variant] + ':' +
                                        str(original.relative_to(ROOT))], cwd=ROOT)
                if variant in refs else original.read_bytes())
        (dest / original.name).write_bytes(data)
    return dest


def segments(path):
    text = path.read_text().split('Segment list:')[1].split('Exports list')[0]
    return {m[1]: {'start': int(m[2], 16), 'end': int(m[3], 16),
                   'size': int(m[4], 16)}
            for m in re.finditer(r'^(\w+)\s+([\dA-F]{6})\s+([\dA-F]{6})\s+([\dA-F]{6})',
                                 text, re.M)}


def build_rom(variant):
    dest = sources(variant)
    assembly = dest / 'nescity_generated.s'
    tool('cc65', *FLAGS, '-o', assembly, dest / 'nescity.c')
    objects = []
    for asm in [assembly, *sorted(dest.glob('city_*.s'))]:
        obj = asm.with_suffix('.o')
        tool('ca65', '-t', 'nes', '-o', obj, asm)
        objects.append(obj)
    tool('ld65', '-C', NESLIB / 'neslib2.cfg', '-m', dest / 'nescity.map',
         '-Ln', dest / 'nescity.lbl', '-o', dest / 'nescity.nes',
         *objects, NESLIB / 'crt0.o', CC65 / 'lib/nes.lib', NESLIB / 'neslib2.lib',
         '-D', 'NES_MAPPER=0', '-D', 'NES_PRG_BANKS=2',
         '-D', 'NES_CHR_BANKS=1', '-D', 'NES_MIRRORING=0')
    seg = segments(dest / 'nescity.map')
    ram_end = max(seg[s]['end'] + 1 for s in ('BSS', 'DATA'))
    assert ram_end <= 0x800, 'Internal RAM overflow'
    assert seg['ZEROPAGE']['end'] < 0x100, 'Zero page overflow'
    rom = (dest / 'nescity.nes').read_bytes()
    assert len(rom) == 16 + 32768 + 8192 and rom[:6] == b'NES\x1a\x02\x01'
    return {'segments': seg, 'ram_end': ram_end,
            'stack_headroom': 0x800 - ram_end,
            'prg_used': sum(seg.get(s, {}).get('size', 0)
                            for s in ('STARTUP', 'ONCE', 'INIT', 'CODE', 'RODATA', 'DATA', 'VECTORS'))}


if __name__ == '__main__':
    import json
    print(json.dumps({v: build_rom(v) for v in VARIANTS}, indent=2))
