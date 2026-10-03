#!/usr/bin/env python3
"""Generate slide values from bundled FPGA evaluation records, without Git history or benchmarking."""
import argparse
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EVAL_REF = '229aed0'
RESULTS = 'Nescity_FPGA/eval/results.json'
RTL = 'Nescity_FPGA/src/cart/city_accelerator_nowater.v'


def at_revision(ref, path):
    if path == RESULTS:
        return (HERE / 'data' / f'{ref}.json').read_bytes()
    if ref == EVAL_REF and path == RTL:
        return (ROOT / RTL).read_bytes()
    raise ValueError(f'No public snapshot for {ref}:{path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    final = json.loads(at_revision(EVAL_REF, RESULTS))
    assert hashlib.sha256(at_revision(EVAL_REF, RTL)).hexdigest() == final['optimized']['rtl_sha256']
    assert not final['hardware_verified'] and not final['quartus_fitter_verified']
    performance = final['performance']
    base = performance['baseline_calc_clocks']
    cycles = performance['optimized_calc_clocks']
    assert final['simulation']['access_checks']['passed']
    assert final['simulation']['access_checks']['writes_per_generation'] == 960
    assert final['simulation']['access_checks']['ordered_consecutive_writes']
    assert final['simulation']['four_state']['passed']
    assert final['mapped_logic_check']['passed']
    full_result = final['simulation']['optimized_full'].split()
    assert full_result[0] == 'PASS'
    test_counts = dict(field.split('=') for field in full_result[1:])
    stages = [base]
    for revision in ['473ef32', '24171f4', '93d2113', EVAL_REF]:
        record = json.loads(at_revision(revision, RESULTS))
        stages.append(record['performance']['optimized_calc_clocks'])
    assert stages == [76804, 13084, 6400, 2299, 1103]
    values = {
        'EvalRef': EVAL_REF,
        'BaseCycles': f'{base:,}',
        'FinalCycles': f'{cycles:,}',
        'BaseMs': f'{base / performance["clock_hz"] * 1000:.4f}',
        'FinalMs': f'{cycles / performance["clock_hz"] * 1000:.4f}',
        'FinalSpeedup': f'{base / cycles:.2f}',
        'CycleReduction': f'{(1 - cycles / base) * 100:.2f}',
        'LastSpeedup': f'{performance["previous_calc_clocks"] / cycles:.2f}',
        'BaseLogic': f'{final["baseline"]["logic_cells"]:,}',
        'FinalLogic': f'{final["optimized"]["logic_cells"]:,}',
        'BaseFF': str(final['baseline']['flip_flops']),
        'FinalFF': str(final['optimized']['flip_flops']),
        'FinalReads': f'{performance["history_seed_reads"] + performance["stream_grid_reads"]:,}',
        'MajorMnineK': str(final['whole_design_memory_estimate']['major_m9k_blocks_total']),
        'DeviceMnineK': str(final['whole_design_memory_estimate']['device_m9k_blocks']),
        'ResetChecks': str(len(final['simulation']['reset_recovery_offsets_calc_clocks'])),
        'TestCases': f'{int(test_counts["cases"]):,}',
        'TestGenerations': f'{int(test_counts["generations"]):,}',
    }
    for index, count in enumerate(stages):
        values[f'Stage{chr(65+index)}Cycles'] = f'{count:,}'
        values[f'Stage{chr(65+index)}Ratio'] = f'{base / count:.3f}'
        values[f'Stage{chr(65+index)}Label'] = f'{base / count:.2f}'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('% Generated from pinned evaluation records.\n' + ''.join(
        f'\\newcommand{{\\{name}}}{{{value}}}\n' for name, value in values.items()))
    print(f'Evidence: {EVAL_REF}; {base}/{cycles} = {base/cycles:.8f}; {len(stages)} stages')


if __name__ == '__main__':
    main()
