#!/usr/bin/env python3
"""Fetch pinned public dependencies locally; do not install system packages."""
from pathlib import Path
import os
import subprocess

HERE = Path(__file__).resolve().parent
PINS = {
    'cc65': ('https://github.com/cc65/cc65.git',
             'e11fb5c39371046ebe25485f984f644c5a0d65d3'),
    '8bitworkshop': ('https://github.com/sehugg/8bitworkshop.git',
                    '6f292bbecfd2865cee2ef9df4665e6fc0bfb907c'),
}


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


if __name__ == '__main__':
    local = HERE / '.tools'
    local.mkdir(exist_ok=True)
    for name, (url, revision) in PINS.items():
        dest = local / name
        if not dest.exists():
            run(['git', 'init', dest])
            run(['git', '-C', dest, 'remote', 'add', 'origin', url])
        head = subprocess.run(['git', '-C', str(dest), 'rev-parse', 'HEAD'],
                              capture_output=True, text=True)
        if head.returncode or head.stdout.strip() != revision:
            if subprocess.check_output(['git', '-C', str(dest), 'status', '--porcelain']):
                raise SystemExit(f'Refusing to overwrite modified dependency: {dest}')
            run(['git', '-C', dest, 'fetch', '--depth', '1', 'origin', revision])
            run(['git', '-C', dest, 'checkout', '--detach', revision])
        print(f'{name}: {revision}', flush=True)
    log = HERE / 'build'
    log.mkdir(exist_ok=True)
    with (log / 'toolchain-build.log').open('w') as output:
        # Libraries need the newly built compiler/assembler; keep these sequential.
        jobs = f'-j{min(os.cpu_count() or 1, 4)}'
        run(['make', jobs, 'bin'], cwd=local / 'cc65', stdout=output, stderr=output)
        run(['make', jobs, '-C', 'libsrc', 'nes', 'sim6502'],
            cwd=local / 'cc65', stdout=output, stderr=output)
    print('Ready. Run python3 Nescity_CPU/eval/verify.py')
