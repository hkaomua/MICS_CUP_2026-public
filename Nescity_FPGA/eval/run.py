#!/usr/bin/env python3
"""Compare accelerator RTL against the unchanged contest C source via CPU MMIO."""
import argparse
import binascii
import ctypes
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = HERE / '.build'
RTL = ROOT / 'Nescity_FPGA/src/cart/city_accelerator_nowater.v'
BASE_REF = 'a614553'
PREVIOUS_REF = '473ef32'
KNOWN = {0: 0x37C2, 256: 0x4963, 512: 0xD350, 768: 0x593A, 1024: 0xEE30, 1280: 0x1FC3}


def run(cmd, **kwargs):
    return subprocess.run([str(x) for x in cmd], check=True, **kwargs)


def tool(name):
    p = shutil.which(name)
    if not p:
        raise SystemExit(f'{name} is required; put your OSS CAD Suite bin directory on PATH')
    return p


def vectors(steps, smoke):
    libpath = BUILD / 'reference.so'
    run([os.environ.get('CC', 'cc'), '-std=c99', '-O2', '-shared', '-fPIC', HERE/'reference.c', '-o', libpath])
    ref = ctypes.CDLL(str(libpath))
    array = ctypes.c_uint8 * 960
    ref.reference_initial.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8)]
    ref.reference_step.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
    ref.reference_initial.restype = ref.reference_step.restype = None
    initial = array(); ref.reference_initial(6502, initial)
    cases = [('official', bytes(initial), steps)]
    # Check every byte, including values outside the normal simulation domain.
    for v in ([0, 1, 14, 15, 24, 25, 34, 35, 55, 62, 63, 78, 79, 100, 101, 255] if smoke else range(256)):
        cases.append((f'uniform-{v}', bytes([v])*960, 2))
    boundary = [0, 1, 2, 14, 15, 24, 25, 34, 35, 49, 50, 55, 57, 58, 62, 63, 78, 79, 100, 101, 254, 255]
    rng = random.Random(0x10_6502)
    for mode in range(4):
        for i in range(2 if smoke else 16):
            palette = [0, 1, 1, 14, 24, 35, 62, 100] if mode == 3 else boundary
            g = bytes(rng.choice(palette) if mode in (0, 3) else rng.randrange(101 if mode == 1 else 256) for _ in range(960))
            cases.append((f'random-{mode}-{i}', g, 3))
    for x,y in [(0,0),(31,0),(0,29),(31,29),(1,1),(30,28),(16,15)]:
        for background, value in [(0,100),(1,63),(25,0),(100,1)]:
            g=bytearray([background]*960); g[y*32+x]=value
            cases.append((f'impulse-{x}-{y}-{background}',g,2))
    for axis in (0,1):
        for shift in range(3):
            g=bytes(boundary[((x if axis == 0 else y)+shift)%len(boundary)] for y in range(30) for x in range(32))
            cases.append((f'stripes-{axis}-{shift}',g,3))
    path=BUILD/'vectors.bin'; crcs={}
    with path.open('wb') as f:
        f.write(struct.pack('<I',len(cases)))
        for name,g,n in cases:
            f.write(struct.pack('<I',n)); f.write(g)
            a=array.from_buffer_copy(g)
            for s in range(n+1):
                if name == 'official' and s in KNOWN:
                    crc=binascii.crc_hqx(bytes(a),0xffff)
                    assert crc == KNOWN[s], (s,hex(crc),hex(KNOWN[s]))
                    crcs[s]=f'{crc:04X}'
                if s != n:
                    ref.reference_step(a); f.write(bytes(a))
    report={'cases':len(cases),'generations':sum(n for _,_,n in cases),'known_crc':crcs,'case_names':[name for name,_,_ in cases]}
    (BUILD/'vectors.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'Generated {report["cases"]} cases / {report["generations"]} generations; CRC {crcs}',flush=True)
    return path


def extra_checks():
    original=subprocess.check_output(['git','show',f'{BASE_REF}:Nescity_FPGA/src/cart/city_accelerator_nowater.v'],cwd=ROOT).decode()
    reference=BUILD/'reference.v'
    reference.write_text(original.replace('city_accelerator_nowater','city_accelerator_reference').replace('city_grid_dpram_dc','city_grid_reference'))
    for name in ['arithmetic','four_state']:
        exe=BUILD/(name+'.vvp')
        cmd=[tool('iverilog'),'-g2012','-s',name+'_tb','-o',exe,HERE/(name+'_tb.sv'),RTL]
        if name=='arithmetic': cmd.append(reference)
        run(cmd)
        result=subprocess.run([tool('vvp'),str(exe),'+vectors='+str(BUILD/'vectors.bin')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        print(result.stdout,end='')
        (BUILD/(name+'.log')).write_text(result.stdout)
        result.check_returncode()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--steps',type=int,default=1280)
    p.add_argument('--smoke',action='store_true')
    source=p.add_mutually_exclusive_group()
    source.add_argument('--baseline',action='store_true')
    source.add_argument('--rtl-ref',help='simulate the accelerator from a Git revision, e.g. '+PREVIOUS_REF)
    p.add_argument('--cpu-half',type=int,default=5)
    p.add_argument('--extra-checks',action='store_true',help='also run Icarus four-state and exhaustive arithmetic checks (requires --steps >= 2)')
    a=p.parse_args()
    if not 1 <= a.steps <= 65535 or a.cpu_half < 1: p.error('steps must be 1..65535; CPU half-period must be positive')
    if a.extra_checks and (a.baseline or a.rtl_ref or a.steps < 2): p.error('--extra-checks requires optimized RTL and at least 2 steps')
    BUILD.mkdir(exist_ok=True)
    path=vectors(a.steps,a.smoke)
    rtl=RTL
    label='baseline' if a.baseline else 'optimized'
    revision=BASE_REF if a.baseline else a.rtl_ref
    if revision:
        commit=subprocess.check_output(['git','rev-parse','--verify','--end-of-options',revision+'^{commit}'],cwd=ROOT,text=True).strip()
        label='baseline' if a.baseline else 'ref-'+commit[:12]
        rtl=BUILD/(label+'.v')
        rtl.write_bytes(subprocess.check_output(['git','show',f'{commit}:Nescity_FPGA/src/cart/city_accelerator_nowater.v'],cwd=ROOT))
    obj=BUILD/('obj-'+label)
    with (BUILD/(label+'-build.log')).open('w') as log:
        cmd=[tool('verilator'),'--cc','--exe','--build','-j','4','-Wno-MULTIDRIVEN',
             '--top-module','city_accelerator_nowater','--Mdir',obj,'-CFLAGS','-O2',rtl,HERE/'sim_main.cpp']
        if a.baseline: cmd.insert(1,'-Wno-fatal')
        run(cmd,stdout=log,stderr=subprocess.STDOUT)
    result=subprocess.run([str(obj/'Vcity_accelerator_nowater'),str(path),str(a.cpu_half)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    print(result.stdout,end='')
    (BUILD/(label+'-simulation.log')).write_text(result.stdout)
    result.check_returncode()
    if a.extra_checks: extra_checks()


if __name__ == '__main__': main()
