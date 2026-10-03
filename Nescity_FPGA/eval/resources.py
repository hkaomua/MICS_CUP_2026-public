#!/usr/bin/env python3
"""Estimate logic with experimental Yosys MAX10 mapping, keeping RAMs unmapped.

Do not use this backend's brams_map_m9k.v: its SINGLE_PORT wrapper discards
A1ADDR (read address) and connects only B1ADDR. Keep all M9K candidates as
$mem_v2 cells, validate their port geometry, and estimate physical blocks from
supported MAX10 configurations. This is not a Quartus fitter/timing report.
"""
import argparse
from collections import Counter
import hashlib
import json
import subprocess
import struct
from run import BASE_REF, PREVIOUS_REF, BUILD, HERE, ROOT, RTL, run, tool


def estimate(label, rtl, history=False, split_grid=False):
    script=BUILD/(label+'-synth.ys')
    output=BUILD/(label+'-synth.json')
    # Run from the build directory; no absolute paths embedded in Yosys commands.
    local=BUILD/(label+'-input.v')
    local.write_bytes(rtl.read_bytes())
    # Follow synth_intel's logic flow, replacing BRAM/FFRAM mapping with an
    # attribute-filtered memory_map so M9K-tagged RAMs keep BOTH addresses.
    script.write_text(f"""read_verilog {local.name}
synth_intel -family max10 -top city_accelerator_nowater -run begin:map_ffram -nobram
opt -fast -mux_undef -undriven -fine -full
memory_map -attr !ramstyle
opt -undriven -fine
techmap -map +/techmap.v
opt -full
clean -purge
setundef -undriven -zero
synth_intel -family max10 -top city_accelerator_nowater -run map_ffs:
stat
write_json {output.name}
write_verilog -noattr {label}-netlist.v
""")
    with (BUILD/(label+'-synth.log')).open('w') as log:
        run([tool('yosys'),'-Q','-T','-s',script.name],cwd=BUILD,stdout=log,stderr=subprocess.STDOUT)
    cells=json.loads(output.read_text())['modules']['city_accelerator_nowater']['cells']
    counts=Counter(c['type'] for c in cells.values())
    allowed={'$mem_v2','$not','dffeas','fiftyfivenm_lcell_comb'}
    assert not set(counts)-allowed, f'unexpected cells (including possible DSP): {counts}'
    memories=[]
    for name,c in cells.items():
        if c['type']=='$mem_v2':
            params={k:int(c['parameters'][k],2) for k in ['ABITS','SIZE','WIDTH','RD_PORTS','WR_PORTS','RD_CLK_ENABLE','WR_CLK_ENABLE']}
            memories.append({'name':name,**params})
    shape_keys=['ABITS','SIZE','WIDTH','RD_PORTS','WR_PORTS','RD_CLK_ENABLE','WR_CLK_ENABLE']
    shape=lambda m:tuple(m[k] for k in shape_keys)
    grid_shape=(10,1024,8,2,2,3,3) if split_grid else (11,2048,8,2,2,3,3)
    history_shape=(5,32,32,1,1,1,1)
    expected=Counter([grid_shape]*(2 if split_grid else 1)+([history_shape] if history else []))
    assert Counter(shape(m) for m in memories)==expected, memories
    for m in memories:
        c=cells[m['name']]
        # Independent CPU/calculator clocks on each grid; one calculator clock
        # and separate addresses on the simple-dual-port history RAM.
        if shape(m)==history_shape:
            assert c['connections']['RD_CLK']==c['connections']['WR_CLK']
            assert c['connections']['RD_ADDR']!=c['connections']['WR_ADDR']
        else:
            assert len(set(c['connections']['RD_CLK']))==2
            assert set(c['connections']['RD_CLK'])==set(c['connections']['WR_CLK'])
    grid_bits=sum(m['SIZE']*m['WIDTH'] for m in memories if shape(m)==grid_shape)
    assert grid_bits==16384
    lut=counts['fiftyfivenm_lcell_comb']; ff=counts['dffeas']
    if label=='optimized':
        # Guardrails for this small 8K-LE part, not a promise that the whole NES fits.
        assert lut<=2000 and ff<=900, (lut,ff)
    return {'rtl_sha256':hashlib.sha256(rtl.read_bytes()).hexdigest(),
            'logic_cells':lut,'flip_flops':ff,'other_not_cells':counts['$not'],
            'unpacked_logic_plus_ff':lut+ff,'dsp_cells':0,'grid_ram_bits':grid_bits,
            'history_logical_bits':1024 if history else 0,
            'expected_m9k_blocks':2+int(history),'mapped_m9k_cells':[],'unmapped_memories':memories}


def check_netlist():
    # Only the exact modes covered by mapped_cells_sim.v are accepted.
    cells=json.loads((BUILD/'optimized-synth.json').read_text())['modules']['city_accelerator_nowater']['cells']
    for name,c in cells.items():
        p=c['parameters']; ports=c['connections']
        if c['type']=='dffeas':
            assert p['power_up'] in ('low','high'), (name,p)
            for k,v in {'prn':'1','asdata':'0','aload':'0','sclr':'0','sload':'0'}.items():
                assert ports[k]==[v], (name,k,ports[k])
        elif c['type']=='fiftyfivenm_lcell_comb':
            assert p['sum_lutc_input']=='datac' and not ports.get('cout',[]), (name,p,ports.get('cout',[]))
    vectors=BUILD/'vectors.bin'
    with vectors.open('rb') as f:
        assert struct.unpack('<II',f.read(8))[1]>=2, 'run.py must generate at least two official steps first'
    exe=BUILD/'mapped-netlist.vvp'
    run([tool('iverilog'),'-g2012','-s','four_state_tb','-o',exe,
         HERE/'four_state_tb.sv',BUILD/'optimized-netlist.v',HERE/'mapped_cells_sim.v'])
    result=subprocess.run([tool('vvp'),str(exe),'+vectors='+str(vectors)],
                          stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (BUILD/'mapped-netlist.log').write_text(result.stdout)
    print(result.stdout,end='')
    result.check_returncode()
    return {'generations':2,'both_banks_checked':True,'passed':True,
            'model':'Mapped LUT/FF logic with checked functional cell modes; generic RAMs; no physical timing.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-netlist',action='store_true',help='also simulate mapped logic and preserved RAMs using existing run.py vectors')
    args=parser.parse_args()
    BUILD.mkdir(exist_ok=True)
    baseline=BUILD/'baseline.v'
    baseline.write_bytes(subprocess.check_output(['git','show',f'{BASE_REF}:Nescity_FPGA/src/cart/city_accelerator_nowater.v'],cwd=ROOT))
    previous=BUILD/'previous.v'
    previous.write_bytes(subprocess.check_output(['git','show',f'{PREVIOUS_REF}:Nescity_FPGA/src/cart/city_accelerator_nowater.v'],cwd=ROOT))
    report={'tool':subprocess.check_output([tool('yosys'),'-V'],text=True).strip(),
            'target':'10M08SAE144C8GES',
            'method':'synth_intel logic mapping with all M9K-tagged memories preserved as $mem_v2; unsafe experimental BRAM wrapper bypassed.',
            'caveat':'Accelerator-only estimate; no Quartus fitting/timing. All M9K counts are inferred from checked RAM shapes, not physically mapped. Previous and baseline estimates are recomputed using this same flow.',
            'baseline':estimate('baseline',baseline),'previous':estimate('previous',previous,history=True),
            'optimized':estimate('optimized',RTL,history=True,split_grid=True)}
    if args.check_netlist: report['mapped_logic_check']=check_netlist()
    (BUILD/'resources.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()
