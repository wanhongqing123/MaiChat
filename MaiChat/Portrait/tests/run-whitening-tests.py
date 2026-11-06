#!/usr/bin/env python3
"""Build portable whitening tests; optionally compare pinned upstream GPU outputs.

No image package is needed for default CPU checks. --baseline-root additionally
uses Pillow and NumPy, the original baseline executable, assets and summary.
--exhaustive compares every possible RGB8 input against shader whiten .25.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-root', type=Path)
    parser.add_argument('--exhaustive', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.exhaustive and not args.baseline_root:
        parser.error('--exhaustive requires --baseline-root')
    root = Path(__file__).resolve().parents[1]
    reports = []
    with tempfile.TemporaryDirectory(prefix='maichat-whitening-test-') as temp_name:
        temp = Path(temp_name)
        binary = temp / 'whitening-tests'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-Wall',
                        '-Wextra', '-Werror', str(root / 'MaiPortraitWhiteningLut.cpp'),
                        str(root / 'tests/MaiPortraitWhiteningLutTests.cpp'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print('Portable whitening invariants and original-GPU color goldens passed.', flush=True)
        if args.baseline_root:
            import numpy as np
            from PIL import Image

            baseline = args.baseline_root.resolve()
            data_binary, data_raw = temp / 'whitening-data-tests', temp / 'lookups.rgb'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-Wall',
                            '-Wextra', '-Werror', str(root / 'tests/MaiPortraitWhiteningLutDataTests.cpp'),
                            '-o', str(data_binary)], check=True)
            subprocess.run([str(data_binary), str(data_raw)], check=True)
            decoded, offset = data_raw.read_bytes(), 0
            for name in ('lookup_gray', 'lookup_origin', 'lookup_skin', 'lookup_light'):
                original = Image.open(baseline / 'upstream/src/res' / (name + '.png')).convert('RGB').tobytes()
                actual = decoded[offset:offset + len(original)]
                if actual != original:
                    raise AssertionError('Embedded LUT differs from original RGB bytes: ' + name)
                offset += len(original)
                print(json.dumps({'asset': name, 'byte_identical': True, 'rgb_bytes': len(original),
                                  'rgb_sha256': hashlib.sha256(actual).hexdigest()}), flush=True)
            if offset != len(decoded):
                raise AssertionError('Unexpected trailing embedded LUT bytes')

            def compare(case, source, gpu_path, weight):
                image = Image.open(source).convert('RGBA')
                raw, output = temp / 'source.rgba', temp / 'output.rgba'
                raw.write_bytes(image.tobytes())
                start = time.monotonic()
                subprocess.run([str(binary), str(raw), str(output), str(weight)], check=True)
                elapsed = time.monotonic() - start
                cpu = np.fromfile(output, dtype=np.uint8).reshape(-1, 4)
                gpu = np.asarray(Image.open(gpu_path).convert('RGBA')).reshape(-1, 4)
                if cpu.shape != gpu.shape:
                    raise AssertionError('GPU dimensions differ from CPU source')
                error = np.abs(cpu[:, :3].astype(np.int16) - gpu[:, :3].astype(np.int16))
                report = {'case': case, 'shader_whiten': weight, 'pixels': len(cpu),
                          'max_rgb_error': int(error.max()),
                          'mean_abs_rgb_error': float(error.mean()),
                          'fraction_rgb_gt1': float((error > 1).mean()),
                          'cpu_seconds': elapsed}
                reports.append(report)
                print(json.dumps(report), flush=True)
                if report['max_rgb_error'] > 1:
                    raise AssertionError('CPU/GPU error exceeds one RGB8 byte')
                if not np.array_equal(cpu[:, 3], np.asarray(image).reshape(-1, 4)[:, 3]):
                    raise AssertionError('CPU alpha changed')

            summary = json.loads((baseline / 'baseline-summary.json').read_text())
            for row in summary['results']:
                if row['blur_alpha'] == 0:
                    compare(row['case'], row['source'], row['output'], row['shader_whiten'])
            if args.exhaustive:
                indices = np.arange(256 ** 3, dtype=np.uint32)
                rgba = np.empty((len(indices), 4), dtype=np.uint8)
                rgba[:, 0], rgba[:, 1], rgba[:, 2], rgba[:, 3] = indices >> 16, indices >> 8, indices, 255
                source, gpu = temp / 'rgb-cube.png', temp / 'rgb-cube-gpu.png'
                Image.fromarray(rgba.reshape(4096, 4096, 4)).save(source)
                del indices, rgba
                subprocess.run([str(baseline / 'build/baseline'), str(baseline / 'upstream/src'),
                                str(source), str(gpu), '2.5', '0'], check=True)
                compare('exhaustive_16777216_RGB8_colors', source, gpu, 0.25)
    if args.report:
        args.report.write_text(json.dumps(reports, indent=2) + '\n')


if __name__ == '__main__':
    main()
