#!/usr/bin/env python3
"""Compare Tilt allocator policies on nested Parquet, with full round-trip checks."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rows', type=int, default=1000000)
    p.add_argument('--row-group-size', type=int, default=10000)
    p.add_argument('--repetitions', type=int, default=4)
    p.add_argument('--build-dir', default='build/release')
    args = p.parse_args()
    if min(args.rows, args.row_group_size, args.repetitions) < 1:
        p.error('counts must be positive')
    import pyarrow as pa
    import pyarrow.parquet as pq
    root = Path(__file__).resolve().parents[1]
    build = (root / args.build_dir).resolve()
    subprocess.run(['cmake', '--build', str(build), '--target', 'tilt', '-j', '2'], check=True, stdout=subprocess.DEVNULL)
    value = (root/'src/runtime/value.cpp').read_text()
    columnar = (root/'src/runtime/columnar.cpp').read_text()
    begin = value.index('Value Value::lista(')
    end = value.index('Value Value::mapa(', begin)
    if value[begin:end].count('std::make_shared<ValueList>()') != 1:
        raise RuntimeError('review current list allocation before building comparison')
    old_value = value[:begin] + value[begin:end].replace('std::make_shared<ValueList>()', 'pooled_object<ValueList>()') + value[end:]
    old_block = '''    // Batch materialization allocates and releases all row maps together.
    // The global synchronized object pool regresses this path (see the
    // value-pool lifecycle benchmark); keep normal shared ownership here.
    Value record;
    record.kind = ValueKind::Mapa;
    record.map_ref() = std::make_shared<ValueMap>();'''
    if columnar.count(old_block) != 1:
        raise RuntimeError('review materialization before building comparison')
    old_columnar = columnar.replace(old_block, '    Value record = Value::mapa();')
    report = {'config': vars(args), 'platform': platform.platform(), 'pyarrow': pa.__version__,
              'compiler': subprocess.check_output(['c++', '--version'], text=True).splitlines()[0],
              'git_head': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
              'source_sha256': {name: hashlib.sha256(text.encode()).hexdigest() for name, text in
                                [('value.cpp', value), ('columnar.cpp', columnar)]}, 'results': {}}
    with tempfile.TemporaryDirectory(prefix='tilt-parquet-allocator-') as directory:
        work = Path(directory)
        for variant, v, c in [('before', old_value, old_columnar), ('after', value, columnar)]:
            (work/f'{variant}-value.cpp').write_text(v)
            (work/f'{variant}-columnar.cpp').write_text(c)
            subprocess.run(['c++', '-std=c++20', '-O3', '-DNDEBUG', '-I', str(root/'src'),
                            str(work/f'{variant}-value.cpp'), str(work/f'{variant}-columnar.cpp'),
                            str(build/'CMakeFiles/tilt.dir/src/main.cpp.o'), str(build/'libtilt_core.a'),
                            '-static-libgcc', '-static-libstdc++', '-ldl', '-pthread', '-o', str(work/variant)], check=True)
        schema = pa.schema([('id', pa.int64()), ('nums', pa.list_(pa.int64())),
                            ('meta', pa.struct([('bucket', pa.int64()), ('label', pa.string())]))])
        with pq.ParquetWriter(work/'input.parquet', schema, compression='gzip') as writer:
            for start in range(0, args.rows, args.row_group_size):
                ids = range(start, min(start+args.row_group_size, args.rows))
                batch = pa.Table.from_pydict({
                    'id': list(ids),
                    'nums': [None if i%11 == 0 else [] if i%7 == 0 else [i, None, i%17, i+1] for i in ids],
                    'meta': [None if i%13 == 0 else {'bucket': i%5, 'label': None if i%19 == 0 else f'label-{i%997}'} for i in ids]
                }, schema=schema)
                writer.write_table(batch, row_group_size=args.row_group_size)
        reference = pq.read_table(work/'input.parquet')
        report['input'] = {'file_bytes': (work/'input.parquet').stat().st_size,
                           'arrow_bytes': reference.nbytes, 'row_groups': pq.ParquetFile(work/'input.parquet').num_row_groups}
        def validate_output():
            actual = pq.read_table(work/'output.parquet')
            if set(actual.column_names) != set(reference.column_names):
                raise RuntimeError(f'column mismatch: {actual.schema}')
            actual = actual.select(reference.column_names)
            # Compare columns to allow top-level required/optional schema differences;
            # types, order, values and nulls must still match exactly.
            if actual.num_rows != reference.num_rows or not all(actual.column(name).equals(reference.column(name)) for name in reference.column_names):
                raise RuntimeError(f'round-trip mismatch: expected {reference.schema}, got {actual.schema}')
        for mode in ('rows', 'columnar'):
            option = ', colunar: verdadeiro' if mode == 'columnar' else ''
            for operation in ('read', 'roundtrip'):
                program = f'pipeline benchmark:\n  passos:\n    - dados = ler_parquet "input.parquet"{option}\n    - imprimir tamanho(dados)\n'
                if mode == 'columnar':
                    program += '    - imprimir dados.metricas_memoria().pico_row_group_bytes\n'
                if operation == 'roundtrip':
                    program += '    - escrever_parquet dados, "output.parquet"\n'
                (work/'run.tilt').write_text(program)
                records = {'before': [], 'after': []}
                for repetition in range(-1, args.repetitions):
                    for variant in (('before', 'after') if repetition%2 == 0 else ('after', 'before')):
                        if (work/'output.parquet').exists():
                            (work/'output.parquet').unlink()
                        start = time.perf_counter_ns()
                        run = subprocess.run(['/usr/bin/time', '-f', '%M', '-o', str(work/'rss.txt'), str(work/variant),
                                              'executar', 'run.tilt'], cwd=work, capture_output=True, text=True, check=True)
                        elapsed = (time.perf_counter_ns()-start)/1e6
                        output = [line for line in run.stdout.strip().splitlines() if not line.startswith("== pipeline ")]
                        if not output or output[0] != str(args.rows):
                            raise RuntimeError(f'row count mismatch: {run.stdout}')
                        if operation == 'roundtrip':
                            validate_output()
                        sample = {'ms': elapsed, 'peak_rss_kib': int((work/'rss.txt').read_text())}
                        if mode == 'columnar':
                            sample['peak_row_group_bytes'] = int(output[1])
                        if repetition >= 0:
                            records[variant].append(sample)
                report['results'][f'{mode}/{operation}'] = {
                    variant: {'median_ms': statistics.median(x['ms'] for x in samples),
                              'median_peak_rss_kib': statistics.median(x['peak_rss_kib'] for x in samples),
                              'max_peak_rss_kib': max(x['peak_rss_kib'] for x in samples), 'samples': samples}
                    for variant, samples in records.items()}
                print(f'completed {mode}/{operation}', file=__import__('sys').stderr, flush=True)
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
