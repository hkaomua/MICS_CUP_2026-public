#!/usr/bin/env python3
"""Attribute measured 6502 cycles to C/inline-assembly functions in a built ROM."""
import argparse
import bisect
from collections import Counter
import csv
import json
import os
import re
from build import BUILD, VARIANTS, run, segments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=VARIANTS, default='optimized')
    parser.add_argument('--steps', type=int, default=1280)
    args = parser.parse_args()
    if not 1 <= args.steps <= 65535:
        parser.error('--steps must be between 1 and 65535')
    dest = BUILD / args.variant
    if not (BUILD / 'rom_runner').exists() or not (dest / 'nescity.nes').exists():
        parser.error('Run verify.py first to build the runner and ROMs')
    raw = dest / 'profile.csv'
    result = run([BUILD / 'rom_runner', dest / 'nescity.nes', dest / 'nescity.lbl',
                  args.steps, dest / 'profile.bin', 'initial'],
                 env=dict(os.environ, NESCITY_PROFILE=str(raw)),
                 capture_output=True, text=True)
    (dest / 'profile.log').write_text(result.stdout + result.stderr)
    labels = {name: int(address, 16) for address, name in re.findall(
        r'^al (\w+) \.(\S+)$', (dest / 'nescity.lbl').read_text(), re.M)}
    functions = re.findall(r'^\.proc\s+(\w+):',
                           (dest / 'nescity_generated.s').read_text(), re.M)
    starts = sorted((labels[name], name) for name in functions)
    module = re.search(r'nescity_generated\.o:\s+CODE\s+Offs=(\w+)\s+Size=(\w+)',
                       (dest / 'nescity.map').read_text())
    code_end = (segments(dest / 'nescity.map')['CODE']['start'] +
                int(module[1], 16) + int(module[2], 16))
    addresses = [address for address, _ in starts]
    counts = Counter()
    with raw.open() as f:
        for row in csv.DictReader(f):
            pc = int(row['address'], 16)
            i = bisect.bisect_right(addresses, pc) - 1
            name = starts[i][1] if i >= 0 and pc < code_end else 'other_assembly_and_runtime'
            counts[name] += int(row['cycles'])
    total = int(re.search(r'^total (\d+)$', result.stdout, re.M)[1])
    assert sum(counts.values()) == total
    report = {'variant': args.variant, 'steps': args.steps, 'total': total,
              'self_cycles_by_function': dict(counts.most_common())}
    (dest / 'profile.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{args.variant}: {total / args.steps:,.1f} cycles/step')
    print('Self cycles exclude called functions; other assembly includes cc65 helpers.')
    for name, cycles in counts.most_common():
        print(f'{cycles / total:6.2%} {cycles / args.steps:12,.1f} {name}')


if __name__ == '__main__':
    main()
