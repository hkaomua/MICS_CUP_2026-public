#!/usr/bin/env python3
"""Estimate accelerator logic with Yosys's experimental MAX10 backend.

This is not a Quartus fitter report. The dual-clock RAM remains a $mem_v2
because this Yosys backend cannot map its two independent write clocks.
"""
from collections import Counter
import hashlib
import json
import subprocess
from run import BASE_REF, PREVIOUS_REF, BUILD, ROOT, RTL, run, tool


def estimate(label, rtl):
    script=BUILD/(label+'-synth.ys')
    output=BUILD/(label+'-synth.json')
    # Run from the build directory; no absolute paths embedded in Yosys commands.
    local=BUILD/(label+'-input.v')
    local.write_bytes(rtl.read_bytes())
    script.write_text(f'read_verilog {local.name}\nsynth_intel -family max10 -top city_accelerator_nowater\nstat\nwrite_json {output.name}\n')
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
    assert len(memories)==1, memories
    m=memories[0]
    assert (m['ABITS'],m['SIZE'],m['WIDTH'],m['RD_PORTS'],m['WR_PORTS'],m['RD_CLK_ENABLE'],m['WR_CLK_ENABLE']) == (11,2048,8,2,2,3,3), m
    lut=counts['fiftyfivenm_lcell_comb']; ff=counts['dffeas']
    if label=='optimized':
        # Guardrails for this small 8K-LE part, not a promise that the whole NES fits.
        assert lut<=1300 and ff<=600, (lut,ff)
    return {'rtl_sha256':hashlib.sha256(rtl.read_bytes()).hexdigest(),
            'logic_cells':lut,'flip_flops':ff,'other_not_cells':counts['$not'],
            'unpacked_logic_plus_ff':lut+ff,'dsp_cells':0,'grid_ram_bits':m['SIZE']*m['WIDTH'],
            'expected_m9k_blocks':2,'unmapped_memories':memories}


def main():
    BUILD.mkdir(exist_ok=True)
    baseline=BUILD/'baseline.v'
    baseline.write_bytes(subprocess.check_output(['git','show',f'{BASE_REF}:Nescity_FPGA/src/cart/city_accelerator_nowater.v'],cwd=ROOT))
    previous=BUILD/'previous.v'
    previous.write_bytes(subprocess.check_output(['git','show',f'{PREVIOUS_REF}:Nescity_FPGA/src/cart/city_accelerator_nowater.v'],cwd=ROOT))
    report={'tool':subprocess.check_output([tool('yosys'),'-V'],text=True).strip(),
            'target':'10M08SAE144C8GES',
            'caveat':'Experimental Yosys mapping, accelerator only; no Quartus fitting/timing. M9K count is inferred from unchanged 2048x8 dual-clock RAM, not mapped by Yosys.',
            'baseline':estimate('baseline',baseline),'previous':estimate('previous',previous),'optimized':estimate('optimized',RTL)}
    (BUILD/'resources.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()
