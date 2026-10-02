#!/usr/bin/env python3
"""Linux list allocation benchmark: same-thread and cross-thread destruction."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--rounds', type=int, default=4)
    p.add_argument('--max-lists', type=int, default=30000)
    p.add_argument('--max-elements', type=int, default=262144)
    p.add_argument('--build-dir', default='build/release')
    args = p.parse_args()
    if min(args.repeats, args.rounds, args.max_lists, args.max_elements) < 1:
        p.error('counts must be positive')
    root = Path(__file__).resolve().parents[1]
    build = root / args.build_dir
    subprocess.run(['cmake', '--build', str(build), '--target', 'tilt_core', '-j', '2'], check=True, stdout=subprocess.DEVNULL)
    source = (root / 'src/runtime/value.cpp').read_text()
    begin = source.index('Value Value::lista(')
    end = source.index('Value Value::mapa(', begin)
    section = source[begin:end]
    pooled = 'pooled_object<ValueList>()'
    standard = 'std::make_shared<ValueList>()'
    if section.count(pooled) + section.count(standard) != 1:
        raise RuntimeError('list factory changed; review variant construction')
    results = {}
    with tempfile.TemporaryDirectory(prefix='tilt-list-handoff-') as directory:
        temp = Path(directory)
        for variant in ('pool', 'standard'):
            new_section = section.replace(standard, pooled) if variant == 'pool' else section.replace(pooled, standard)
            cpp = temp / (variant + '.cpp')
            cpp.write_text(source[:begin] + new_section + source[end:])
            subprocess.run(['c++', '-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Werror', '-I', str(root/'src'),
                            str(root/'benchmarks/list_handoff_benchmark.cpp'), str(cpp), str(build/'libtilt_core.a'),
                            '-pthread', '-ldl', '-o', str(temp/variant)], check=True)
        for size in (0, 2, 32, 256):
            count = min(args.max_lists, max(1, args.max_elements // max(1, size)))
            for threads in (1, 2, 4):
                for mode in (('local',) if threads == 1 else ('local', 'handoff')):
                    samples = {'pool': [], 'standard': []}
                    for run in range(args.rounds):
                        for variant in (('pool', 'standard') if run % 2 == 0 else ('standard', 'pool')):
                            output = subprocess.check_output([str(temp/variant), str(count), str(size), str(threads), str(args.repeats), mode], text=True)
                            samples[variant].extend([float(c), float(d), int(rss)] for c,d,rss in (line.split() for line in output.splitlines()))
                    results[f'{size}/{threads}/{mode}'] = {'lists_per_thread': count, **{
                        variant: {'median_cycle_ms': statistics.median(c+d for c,d,_ in data),
                                  'median_create_ms': statistics.median(c for c,_,_ in data),
                                  'median_destroy_ms': statistics.median(d for _,d,_ in data),
                                  'peak_rss_kib': max(rss for _,_,rss in data), 'samples': data}
                        for variant, data in samples.items()}}
    print(json.dumps({'config': vars(args), 'sample_columns': ['max_create_ms', 'max_destroy_ms', 'peak_rss_kib'], 'results': results}, indent=2))

if __name__ == '__main__':
    main()
