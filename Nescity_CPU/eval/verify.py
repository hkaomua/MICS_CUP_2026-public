#!/usr/bin/env python3
"""Compare real 6502 ROM execution against the independent Linux model."""
import argparse
import binascii
import json
import random
import re
import subprocess
from build import BUILD, CC65, HERE, ROOT, PREVIOUS_REF, VARIANTS, build_rom, run
from bootstrap import PINS


def compare(actual, expected, name):
    if actual != expected:
        i = next((i for i, pair in enumerate(zip(actual, expected))
                  if pair[0] != pair[1]), min(len(actual), len(expected)))
        raise AssertionError(f'{name}: mismatch at step {i // 960}, '
                             f'x={i % 32}, y={i % 960 // 32}; '
                             f'lengths {len(actual)}/{len(expected)}')


def reference(name, steps, initial='initial', water=0):
    output = BUILD / f'{name}.reference.bin'
    run([BUILD / 'reference', steps, output, initial, water])
    return output.read_bytes()


def emulate(variant, name, steps, expected, initial='initial', water=0):
    dest = BUILD / variant
    output = dest / f'{name}.bin'
    result = run([BUILD / 'rom_runner', dest / 'nescity.nes',
                  dest / 'nescity.lbl', steps, output, initial, water],
                 capture_output=True, text=True)
    (dest / f'{name}.log').write_text(result.stdout + result.stderr)
    compare(output.read_bytes(), expected, f'{variant}/{name}')
    # Check the real assembly CRC against a separate host implementation too.
    crcs = [(int(s), int(c, 16)) for s, c in re.findall(r'^crc (\d+) ([0-9A-F]+)$',
                                                     result.stdout, re.M)]
    assert len(crcs) == steps + 1
    for s, crc in crcs:
        assert crc == binascii.crc_hqx(expected[s * 960:(s + 1) * 960], 0xffff)
    metrics = {m[1]: int(m[2]) for m in re.finditer(
        r'^(total|min_csp|min_hsp|ram_end|nmi) (\d+)$', result.stdout, re.M)}
    metrics['cycles'] = [int(c) for c in re.findall(r'^cycles \d+ (\d+)$',
                                                   result.stdout, re.M)]
    metrics['stack_margin'] = metrics['min_csp'] - metrics['ram_end']
    assert metrics['stack_margin'] >= 0
    assert metrics['min_hsp'] >= 0xe0, 'Hardware stack reaches palette buffer'
    return metrics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--steps', type=int, default=1280)
    parser.add_argument('--quick', action='store_true', help='5 steps and fewer random cases')
    args = parser.parse_args()
    steps = 5 if args.quick else args.steps
    if not 5 <= steps <= 65535:
        parser.error('--steps must be between 5 and 65535')
    for name, (_, revision) in PINS.items():
        actual = subprocess.check_output(['git', '-C', str(HERE / '.tools' / name),
                                          'rev-parse', 'HEAD'], text=True).strip()
        assert actual == revision, f'Run bootstrap.py: wrong {name} revision'
        assert not subprocess.check_output(['git', '-C', str(HERE / '.tools' / name),
                                            'diff', 'HEAD']), f'Modified dependency: {name}'
    print('Building three ROMs with identical flags...', flush=True)
    resources = {v: build_rom(v) for v in VARIANTS}
    (BUILD / 'resources.json').write_text(json.dumps(resources, indent=2) + '\n')
    simsrc = CC65 / 'src/sim65'
    run(['cc', '-O2', '-I', simsrc, '-I', CC65 / 'src/common',
         HERE / 'rom_runner.c', simsrc / '6502.c', '-o', BUILD / 'rom_runner'])
    run(['cc', '-O2', HERE / 'reference.c', '-o', BUILD / 'reference'])
    print('Checking all /65 inputs and arithmetic tables on the actual ROM...', flush=True)
    dest = BUILD / 'optimized'
    arithmetic = run([BUILD / 'rom_runner', dest / 'nescity.nes', dest / 'nescity.lbl',
                      0, '-', 'math'], capture_output=True, text=True)
    (dest / 'math.log').write_text(arithmetic.stdout + arithmetic.stderr)
    assert arithmetic.stdout == 'math_average 6501\nmath_force 326\nmath_capacity 101\n'
    # Check actual generated code, including the new helpers and row copies.
    assembly = (dest / 'nescity_generated.s').read_text()
    hot_functions = ('sim_step', 'compute_next_row', 'city_slide_column',
                     'city_compute_average', 'city_finish_cell', 'city_reset_columns',
                     'city_compute_row', 'city_prepare_rows', 'city_mark_house_columns',
                     'city_advance_house_columns', 'city_duplicate_house_columns', 'city_copy32',
                     'copy_row_to', 'copy_row_from', 'wrap_y')
    for function in hot_functions:
        body = assembly.split('.proc\t_' + function + ':', 1)[1].split('.endproc', 1)[0]
        heavy_calls = re.findall(r'\b(?:jsr|jmp)\s+(\w*(?:mul|div|mod)\w*)', body)
        assert not heavy_calls, (function, heavy_calls)
    expected = reference('initial', steps)
    official = {0: 0x37c2, 1: 0xe612, 2: 0x1330, 3: 0x6bc7, 4: 0xfe83, 5: 0xd764,
                256: 0x4963, 512: 0xd350, 768: 0x593a, 1024: 0xee30, 1280: 0x1fc3}
    for s, crc in official.items():
        if s <= steps:
            assert binascii.crc_hqx(expected[s * 960:(s + 1) * 960], 0xffff) == crc
    metrics = {}
    for variant in VARIANTS:
        print(f'{variant}: comparing {steps} steps, all 960 cells at every step...', flush=True)
        metrics[variant] = emulate(variant, 'initial', steps, expected)
    # Complete program path, reset -> initialization -> NMI -> CRC -> redraw.
    boot = {}
    for variant in VARIANTS:
        boot[variant] = emulate(variant, 'boot', 5, expected[:6 * 960], 'boot')
        assert boot[variant]['nmi'] > 0
    print('Testing every uniform value, thresholds, torus seams, random grids, and water penalties...',
          flush=True)
    scenarios = [(f'uniform-{v}', bytes([v]) * 960, 0) for v in range(101)]
    boundary = [0, 1, 2, 14, 15, 24, 25, 34, 35, 49, 50, 62, 63, 78, 79, 100]
    scenarios += [('thresholds', bytes(boundary[(x + y * 7) % len(boundary)]
                                    for y in range(30) for x in range(32)), 0),
                  ('torus-corners', bytes((100 if x in (0, 31) and y in (0, 29) else
                                          1 if x in (1, 30) or y in (1, 28) else 24)
                                         for y in range(30) for x in range(32)), 0),
                  ('sea-gap', bytes(62 if x in (0, 17, 31) else 0
                                    for y in range(30) for x in range(32)), 0),
                  ('single-land', bytes(1 if x == 31 and y == 29 else 0
                                        for y in range(30) for x in range(32)), 0)]
    rng = random.Random(6502)
    for n in range(4 if args.quick else 32):
        scenarios.append((f'random-{n}', bytes(rng.randrange(101) for _ in range(960)),
                          (0, 1, 12, 255)[n % 4]))
    # Exercise biased-byte subtraction and exact early exits on distributions
    # unlike the supplied initial map, including sparse mountains amid sea.
    for n in range(4 if args.quick else 32):
        values = ([0] * 12 + [1, 1, 14, 24, 34, 62, 78, 100] if n % 2 else
                  [1] * 12 + [0, 14, 15, 24, 25, 34, 35, 49, 50, 62, 63, 78, 79, 100])
        scenarios.append((f'sparse-{n}', bytes(rng.choice(values) for _ in range(960)),
                          (0, 127, 128, 129, 134, 135, 254, 255)[n % 8]))
    # Repeated uniform columns must reuse the cache; alternating values must
    # invalidate it, including at the left/right seam and on the next row.
    for period in (1, 2, 5, 7):
        scenarios.append((f'column-stripes-{period}',
                          bytes(boundary[(x // period) % len(boundary)]
                                for y in range(30) for x in range(32)), 0))
    # A mountain with zero/one house must stay 1 even with strong sea/shop
    # influence. Include the two-house boundary, where it may develop, and
    # place the center at a torus seam. Compare against the original rules.
    offsets = sorted(((dx, dy) for dy in range(-2, 3) for dx in range(-2, 3)
                      if dx or dy), key=lambda xy: (abs(xy[0]) + abs(xy[1]), xy))
    for houses in (0, 1, 2):
        for seas in (0, 1, 3, 4, 12, 22):
            data = bytearray([63] * 960)
            data[0] = 1
            for i, (dx, dy) in enumerate(offsets):
                data[(dy % 30) * 32 + dx % 32] = (62 if i < houses else
                                                 0 if i < houses + seas else 63)
            scenarios.append((f'mountain-{houses}-houses-{seas}-seas', bytes(data), 0))
    # Exercise every lazy-window gap length with fixed sea and mountain runs.
    # Houses around rows 29/0/1 also test the vertical counter update before
    # row writeback, including saved top rows and resets between generations.
    for filler in (0, 1):
        for gap in range(32):
            data = bytearray([filler] * 960)
            for y in (0, 1, 14, 28, 29):
                data[y * 32] = 100
                data[y * 32 + gap] = 63
                data[((y + 1) % 30) * 32 + (gap + 1) % 32] = 62
            scenarios.append((f'lazy-gap-{filler}-{gap}', bytes(data), gap % 2 * 255))
    # Entire sea rows still need to advance the auxiliary house counts.
    for offset in range(4):
        data = bytearray(960)
        for y in range(30):
            if (y + offset) % 4:
                for x in range(32):
                    data[y * 32 + x] = (62 if (x + y) % 13 == 0 else
                                        100 if (x + y) % 17 == 0 else 1)
        scenarios.append((f'lazy-sea-rows-{offset}', bytes(data), 0))
    # Mixed vertical samples around byte-carry boundaries. These bypass the
    # uniform-column shortcut and exercise both plain and weighted words.
    carry_patterns = [(63, 64, 0, 64, 64), (64, 64, 0, 64, 64),
                      (0, 27, 100, 28, 0), (0, 28, 100, 28, 0),
                      (0, 63, 1, 63, 0), (1, 63, 1, 63, 0),
                      (0, 100, 100, 100, 67), (0, 100, 100, 100, 68),
                      (100, 100, 99, 100, 100), (100, 100, 100, 100, 99)]
    for n, values in enumerate(carry_patterns):
        for shift in (0, 2):
            data = bytes(values[(y + shift) % 5] for y in range(30) for x in range(32))
            scenarios.append((f'population-carry-{n}-{shift}', data, 0))
    margin = metrics['optimized']['stack_margin']
    for name, data, water in scenarios:
        initial = BUILD / 'scenario.bin'
        initial.write_bytes(data)
        ref = reference(name, 2, initial, water)
        checked = emulate('optimized', name, 2, ref, initial, water)
        margin = min(margin, checked['stack_margin'])
    # A negative check ensures the stack guard actually rejects a collision.
    source_labels = BUILD / 'optimized/nescity.lbl'
    text = source_labels.read_text()
    start = resources['optimized']['segments']['BSS']['start']
    text = re.sub(r'^al [0-9A-Fa-f]+ \.__BSS_SIZE__$',
                  f'al {0x800 - start:06X} .__BSS_SIZE__', text, flags=re.M)
    bad = BUILD / 'overlapping.lbl'
    bad.write_text(text)
    result = subprocess.run([str(BUILD / 'rom_runner'),
                             str(BUILD / 'optimized/nescity.nes'), str(bad), '1',
                             str(BUILD / 'rejected.bin'), 'initial'], capture_output=True)
    assert result.returncode != 0 and b'C stack overlaps' in result.stderr
    # Only the one authorized application source may differ from the baseline.
    changed = subprocess.check_output(['git', 'diff', '--name-only', 'baseline', '--',
                                       'Nescity_CPU/8bitworkshop', 'Nescity_FPGA',
                                       'linux_city.c', 'README.txt', 'Nescity_CPU/README.txt'],
                                      cwd=ROOT, text=True).splitlines()
    assert changed == ['Nescity_CPU/8bitworkshop/nescity.c'], changed
    base = metrics['baseline']['total']
    previous = metrics['previous']['total']
    opt = metrics['optimized']['total']
    summary = {'steps': steps, 'additional_scenarios': len(scenarios),
               'dependencies': {n: p[1] for n, p in PINS.items()},
               'resources': resources, 'cpu': metrics, 'boot': boot,
               'previous_ref': PREVIOUS_REF, 'speedup_over_previous': previous / opt,
               'arithmetic_cases': {'average': 6501, 'force': 326, 'capacity': 101},
               'hot_functions_without_general_mul_div_mod': hot_functions,
               'speedup': base / opt, 'minimum_optimized_stack_margin': margin,
               'official_crcs': {str(s): f'{c:04X}' for s, c in official.items() if s <= steps}}
    (BUILD / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'PASS: {steps} steps + {len(scenarios)} scenarios; '
          f'CPU speedup {base / opt:.3f}x original / {previous / opt:.3f}x previous; '
          f'stack margin >= {margin} bytes.\n'
          f'ROM: {BUILD / "optimized/nescity.nes"}', flush=True)


if __name__ == '__main__':
    main()
