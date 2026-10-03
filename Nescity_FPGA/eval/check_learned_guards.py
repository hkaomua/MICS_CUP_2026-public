#!/usr/bin/env python3
"""Exercise exact-identity, mid-run edits, reset and learned-path abort recovery."""
import argparse
import ctypes
import struct
import subprocess
from learned import reference
from run import BUILD, HERE, RTL, run, tool


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpu-half', type=int, default=5)
    args = parser.parse_args()
    ref = reference()
    array = ctypes.c_uint8 * 960
    original = array()
    ref.reference_initial(6502, original)
    # mode, initial grid, CPU write index / argument (or abort offset), trained.
    cases = []
    for index in (0, 500, 959):
        changed = bytearray(original)
        changed[index] ^= 128  # upper bit must also participate in the identity check
        cases.append((3, changed, 0, 0, False))
    # A later standard checkpoint loaded as generation 0 is not the trained start.
    checkpoint = array.from_buffer_copy(original)
    for _ in range(86):
        ref.reference_step(checkpoint)
    cases.append((3, checkpoint, 0, 0, False))
    for index in (0, 500, 959):
        cases.append((0, original, index, 255, True))
    at90 = array.from_buffer_copy(original)
    for _ in range(90):
        ref.reference_step(at90)
    cases.append((0, original, 500, at90[500], True))
    cases.append((1, original, 0, 0, True))
    cases.append((4, original, 0, 0, True))
    for offset in (5, 6, 7, 8, 55, 120, 4130, 4131):
        cases.append((2, original, 0, offset, True))
    path = BUILD / 'learned-guards.bin'
    with path.open('wb') as out:
        out.write(struct.pack('<I', len(cases)))
        for mode, initial, index, arg, trained in cases:
            out.write(struct.pack('<5I', mode, 90, index, arg, trained))
            grid = array.from_buffer_copy(initial)
            out.write(bytes(grid))
            for _ in range(90):
                ref.reference_step(grid)
                out.write(bytes(grid))
            if mode == 0:
                grid[index] = arg
            # Cross generation 86 again after resets, detecting stale validity.
            post = 100 if mode in (1, 2, 4) else 12
            out.write(struct.pack('<I', post))
            for _ in range(post):
                ref.reference_step(grid)
                out.write(bytes(grid))
    obj = BUILD / 'obj-learned-guards'
    with (BUILD / 'learned-guards-build.log').open('w') as log:
        run([tool('verilator'), '--cc', '--exe', '--build', '-j', '4', '-Wno-MULTIDRIVEN',
             '--top-module', 'city_accelerator_nowater', '--Mdir', obj, '-CFLAGS', '-O2',
             RTL, HERE / 'learned_guards.cpp'], stdout=log, stderr=subprocess.STDOUT)
    result = subprocess.run([str(obj / 'Vcity_accelerator_nowater'), str(path), str(args.cpu_half)],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (BUILD / f'learned-guards-{args.cpu_half}.log').write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()


if __name__ == '__main__':
    main()
