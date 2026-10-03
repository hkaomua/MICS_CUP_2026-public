#!/usr/bin/env python3
"""Fit an exact-on-accepted-samples temporal rule model; no neural network.

Run: python3 train_patterns.py training.bin trained.json [--device cpu]
The model may abstain. Unmodeled cells use the original exact update rule.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def select_row(eligible):
    # Lookup + predicted write = 2 cycles; exact cell including lookup = 7.
    # Restarting the 5-column cache costs INIT + 25 reads + drain = 27 cycles.
    states = {False: (0, [])}
    for ok in eligible:
        new = {}
        for cached, (cost, path) in states.items():
            options = [(True, cost + 7 + (0 if cached else 27), path + [False])]
            if ok:
                options.append((False, cost + 2, path + [True]))
            for cache, value, route in options:
                if cache not in new or value < new[cache][0]:
                    new[cache] = (value, route)
        states = new
    return min(states.values(), key=lambda x: x[0])


def main():
    import numpy as np
    import torch

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('frames')
    parser.add_argument('output')
    parser.add_argument('--device', choices=['cuda', 'cpu'], default='cuda')
    args = parser.parse_args()
    blob = Path(args.frames).read_bytes()
    raw = np.frombuffer(blob, dtype=np.uint8).reshape(-1, 960).copy()
    assert raw.shape == (1281, 960) and int(raw[0].max()) <= 100
    torch.set_num_threads(2)
    if args.device == 'cuda':
        torch.cuda.set_per_process_memory_fraction(0.03, 0)
    samples = torch.from_numpy(raw).to(args.device)
    times = torch.arange(1281, device=args.device, dtype=torch.int16)[:, None]
    # First generation after the last disagreement with the final observed value.
    # This is an empirical finite-horizon rule, not proof of eternal convergence.
    last = torch.where(samples != samples[-1], times, torch.full_like(times, -1)).amax(dim=0)
    settle = (last + 1).cpu().tolist()
    final = raw[-1].tolist()
    candidates = sorted(set([0] + [x for x in settle if x < 1280]))
    best = None
    scores = []
    for start in candidates:
        mask, steady = [], 0
        for y in range(30):
            eligible = [settle[i] <= start and final[i] in (0, 1, 100)
                        for i in range(y * 32, (y + 1) * 32)]
            cost, row = select_row(eligible)
            steady += cost
            mask.extend(row)
        steady += 5  # start handshake, first model lookup wait, finish
        total = 6400 * start + steady * (1280 - start) + 961  # 960 reads + final compare
        scores.append({'start': start, 'predicted_cells': sum(mask),
                       'steady_cycles_estimate': steady, 'total_cycles_estimate': total})
        if best is None or total < best[0]:
            best = (total, start, mask, steady)
    total, start, mask, steady = best
    pred_mask = torch.tensor(mask, device=args.device, dtype=torch.bool)
    accepted = samples[start + 1:, pred_mask]
    correct = int((accepted == samples[-1, pred_mask]).sum().item())
    count = accepted.numel()
    assert correct == count
    classes = {0: 1, 1: 2, 100: 3}
    words = [int(raw[0, i]) | ((classes[final[i]] if mask[i] else 0) << 7)
             for i in range(960)] + [0] * 64
    assert all(0 <= word < 512 for word in words)
    report = {
        'model_kind': 'selective temporal constant-rule classifier (not a neural network)',
        'training_sha256': hashlib.sha256(blob).hexdigest(),
        'training_seed': 6502, 'training_generations': [0, 1280],
        'prediction_source_generation_min': start,
        'prediction_source_generation_max_exclusive': 1280,
        'predicted_cells_per_enabled_generation': sum(mask),
        'accepted_predictions': count, 'correct_accepted_predictions': correct,
        'accepted_precision': 1.0, 'training_coverage': count / (1280 * 960),
        'abstentions': 1280 * 960 - count, 'standalone_full_coverage': False,
        'rom_bits': 9216, 'rom_words_9bit': words,
        'steady_cycles_estimate': steady, 'total_cycles_estimate': total,
        'predicted_value_counts': dict(Counter(final[i] for i in range(960) if mask[i])),
        'dense_memorizer': {
            'training_precision': 1.0, 'training_coverage': 1.0, 'raw_bytes': len(blob),
            'unique_cell_timelines': len(set(raw[:, i].tobytes() for i in range(960)))},
        'device': torch.cuda.get_device_name(0) if args.device == 'cuda' else 'CPU',
        'torch_version': torch.__version__, 'scores': scores}
    Path(args.output).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('rom_words_9bit', 'scores')}, indent=2))


if __name__ == '__main__':
    main()
