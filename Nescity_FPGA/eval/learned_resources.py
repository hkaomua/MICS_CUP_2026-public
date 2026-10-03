#!/usr/bin/env python3
"""Compare logic with memories retained: Yosys MAX10 cannot map initialized ROM.

Both revisions use the same flow. No memory contents are discarded. M9K block
counts are geometry estimates, not a successful Quartus fit or Yosys BRAM map.
"""
from collections import Counter
import hashlib
import json
import subprocess
from run import BUILD, ROOT, RTL, run, tool


def estimate(label, rtl):
    local = BUILD / (label + '-input.v')
    local.write_bytes(rtl)
    output = BUILD / (label + '-resources.json')
    script = BUILD / (label + '-resources.ys')
    # Follow synth_intel, retaining $mem_v2 in place of map_bram/map_ffram's
    # memory_map. The bundled BRAM techmap rejects initialized ROM's INIT param.
    script.write_text(f'''read_verilog {local.name}
read_verilog -sv -lib +/intel/max10/cells_sim.v
read_verilog -sv -lib +/intel/common/m9k_bb.v
hierarchy -check -top city_accelerator_nowater
proc
check
flatten
tribuf -logic
deminout
opt_expr
opt_clean
check
opt -nodffe -nosdff
fsm
opt
wreduce
peepopt
opt_clean
techmap -map +/cmp2lut.v -D LUT_WIDTH=4
opt_expr
opt_clean
opt
memory -nomap
opt_clean
opt -fast -mux_undef -undriven -fine -full
opt -undriven -fine
techmap -map +/techmap.v
opt -full
clean -purge
setundef -undriven -zero
synth_intel -family max10 -top city_accelerator_nowater -run map_ffs:
stat
write_json {output.name}
''')
    with (BUILD / (label + '-resources.log')).open('w') as log:
        run([tool('yosys'), '-Q', '-T', '-s', script.name], cwd=BUILD, stdout=log, stderr=subprocess.STDOUT)
    cells = json.loads(output.read_text())['modules']['city_accelerator_nowater']['cells']
    counts = Counter(c['type'] for c in cells.values())
    assert not set(counts) - {'$mem_v2', '$not', 'dffeas', 'fiftyfivenm_lcell_comb'}, counts
    memories = []
    for name, c in cells.items():
        if c['type'] != '$mem_v2':
            continue
        params = {k: int(c['parameters'][k], 2) for k in
                  ['ABITS', 'SIZE', 'WIDTH', 'RD_PORTS', 'WR_PORTS', 'RD_CLK_ENABLE', 'WR_CLK_ENABLE']}
        memories.append({'name': name, **params})
    shapes = sorted((m['SIZE'], m['WIDTH'], m['RD_PORTS'], m['WR_PORTS'], m['RD_CLK_ENABLE']) for m in memories)
    expected = [(2048, 8, 2, 2, 3)]
    if label == 'learned':
        expected.insert(0, (1024, 9, 1, 0, 1))
    assert shapes == expected, shapes
    lut, ff = counts['fiftyfivenm_lcell_comb'], counts['dffeas']
    # Accelerator guardrails, not total NES device utilization.
    assert lut <= 1600 and ff <= 650, (lut, ff)
    return {'rtl_sha256': hashlib.sha256(rtl).hexdigest(), 'logic_cells': lut,
            'flip_flops': ff, 'dsp_cells': 0, 'other_not_cells': counts['$not'],
            'unmapped_memories': memories, 'expected_m9k_blocks': 2 + (label == 'learned')}


def main():
    BUILD.mkdir(exist_ok=True)
    stable = subprocess.check_output(['git', 'show', '24171f4:Nescity_FPGA/src/cart/city_accelerator_nowater.v'], cwd=ROOT)
    report = {'tool': subprocess.check_output([tool('yosys'), '-V'], text=True).strip(),
              'target': '10M08SAE144C8GES',
              'caveat': 'Logic-only mapping with RAM/ROM retained as $mem_v2. Experimental MAX10 backend rejects initialized ROM INIT; no Quartus fit/timing or mapped M9K claim.',
              'stable': estimate('stable', stable), 'learned': estimate('learned', RTL.read_bytes())}
    (BUILD / 'learned-resources-summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
